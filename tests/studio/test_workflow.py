"""Integration tests execute actual compiled modules in a separate runtime process."""
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from plc_build import ROOT, compile_project
from plc_project import prepare, ProjectError
from plc_studio import RuntimeProcess, Studio, handler_for
from http.server import ThreadingHTTPServer
DEMO=json.loads((ROOT/'examples/windows_demo/project.json').read_text())

def simple(source, **task_options):
    return {'schema':1,'name':'Integration test','data_blocks':[{'number':1,'fields':[
        {'name':'A','type':'DINT','offset':0,'initial':0},
        {'name':'Flag','type':'BOOL','offset':4,'bit':0,'initial':False},
        {'name':'Other','type':'BOOL','offset':4,'bit':1,'initial':True}
    ]}],'globals':[],'instances':[],'blocks':[], 'tasks':[{'ob':1,'kind':'main','period_ms':10,'priority':1,'watchdog_ms':1000,'networks':[{'title':'Test','language':'SCL','source':source}],**task_options}]}

def values(snapshot):return {t['name']:t['value'] for t in snapshot['tags']}

def wait_for(runtime,predicate,timeout=3):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        result=runtime.command('STATUS')
        if predicate(result):return result
        time.sleep(.01)
    raise AssertionError('Runtime did not reach expected state: '+str(result))

class FrontendTests(unittest.TestCase):
    def test_requires_one_main_ob(self):
        p=copy.deepcopy(DEMO);p['tasks']=[t for t in p['tasks'] if t['ob']!=1]
        with self.assertRaisesRegex(ProjectError,'main OB1'):prepare(p)
    def test_rejects_duplicate_db_and_overlap(self):
        p=copy.deepcopy(DEMO);p['instances'][0]['db']=1
        with self.assertRaisesRegex(ProjectError,'Duplicate'):prepare(p)
        p=copy.deepcopy(DEMO);p['data_blocks'][0]['fields'][1]['bit']=0
        with self.assertRaisesRegex(ProjectError,'Overlapping'):prepare(p)
    def test_absolute_db_addresses_resolve(self):
        p=simple('DB1.DBD0 := 7;');result=prepare(p)
        self.assertIn('DB1.A := 7;',result.source)
    def test_unsupported_stl_fails_instead_of_being_ignored(self):
        p=simple('');p['tasks'][0]['networks'][0].update(language='STL',source='JU Label')
        with self.assertRaisesRegex(ProjectError,'unsupported instruction'):prepare(p)
    def test_cyclic_period_rejects_zero(self):
        p=copy.deepcopy(DEMO);p['tasks'][2]['period_ms']=0
        with self.assertRaisesRegex(ProjectError,'period'):prepare(p)
    def test_ladder_requires_output(self):
        p=simple('');p['tasks'][0]['networks']=[{'language':'LAD','branches':[[]],'coils':[]}]
        with self.assertRaisesRegex(ProjectError,'coil or a block call'):prepare(p)

class RuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.demo=compile_project(DEMO)
    def setUp(self):
        self.runtime=RuntimeProcess();self.runtime.ensure(self.demo['runtime']);self.addCleanup(self.runtime.close)
    def load(self,result=None):return self.runtime.command('LOAD\t'+(result or self.demo)['module'])
    def write(self,snapshot,name,value):
        tid=next(t['id'] for t in snapshot['tags'] if t['name']==name)
        return self.runtime.command(f'SET\t{tid}\t{int(value) if isinstance(value,bool) else value}')
    def test_all_five_languages_compile_load_run_and_monitor(self):
        self.assertEqual(self.load()['state'],'STOP');self.runtime.command('RUN')
        s=wait_for(self.runtime,lambda s:values(s)['DB1.CyclicCount']>=2);v=values(s)
        self.assertEqual(v['DB1.StartupCount'],1)
        self.assertEqual(v['DB1.Speed'],100)
        self.assertEqual(v['DB1.Scans'],v['DB1.NativeCCount'])
        self.assertEqual(v['DB1.Scans'],v['DB1.NativeCppCount'])
        self.assertGreater(v['DB1.Scans'],v['DB1.CyclicCount'])
        self.assertEqual(len(s['networks']),10)
        self.write(s,'DB1.Start',True);s=wait_for(self.runtime,lambda s:values(s)['MotorOutput'])
        self.write(s,'DB1.Start',False);self.assertTrue(values(self.runtime.command('STATUS'))['DB1.Run'])
        self.write(s,'DB1.Stop',True);wait_for(self.runtime,lambda s:not values(s)['MotorOutput'])
        s=self.runtime.command('STOP');self.assertFalse(values(s)['MotorOutput'])
        count=values(s)['DB1.Scans'];time.sleep(.04);self.assertEqual(values(self.runtime.command('STATUS'))['DB1.Scans'],count)
        self.runtime.command('RUN');wait_for(self.runtime,lambda s:values(s)['DB1.StartupCount']==2)
    def test_running_load_is_rejected_and_old_program_kept(self):
        self.load();self.runtime.command('RUN')
        with self.assertRaisesRegex(ProjectError,'STOP'):self.load()
        self.assertEqual(self.runtime.command('STATUS')['state'],'RUN')
    def test_invalid_load_preserves_current_program(self):
        old=self.load()
        with self.assertRaises(ProjectError):self.runtime.command('LOAD\t'+str(ROOT/'missing.dll'))
        self.assertEqual(self.runtime.command('STATUS')['build_id'],old['build_id'])
    def test_reset_restores_initials_and_step_runs_main_only(self):
        s=self.load();self.write(s,'DB1.Command',70);s=self.runtime.command('STEP');v=values(s)
        self.assertEqual(v['DB1.Speed'],140);self.assertEqual(v['DB1.StartupCount'],0);self.assertEqual(v['DB1.CyclicCount'],0)
        s=self.runtime.command('RESET');self.assertEqual(values(s)['DB1.Command'],50);self.assertEqual(values(s)['DB1.Scans'],0)
    def test_online_write_validation(self):
        s=self.load()
        with self.assertRaisesRegex(ProjectError,'out of range'):self.write(s,'DB1.Command',2147483648)
        with self.assertRaisesRegex(ProjectError,'outputs are held'):self.write(s,'MotorOutput',True)
        self.assertEqual(values(self.runtime.command('STATUS'))['DB1.Command'],50)
    def test_independent_fb_instance_dbs(self):
        p=copy.deepcopy(DEMO);p['instances'].append({'name':'Motor2','type':'FB_Motor','db':102})
        p['tasks']=[{'ob':1,'kind':'main','period_ms':10,'networks':[{'language':'SCL','source':'Motor1(Enable := TRUE); Motor2(Enable := FALSE);'}]}]
        result=compile_project(p);self.load(result);self.runtime.command('RUN')
        s=wait_for(self.runtime,lambda s:values(s)['Motor1.RunScans']>=3);self.assertEqual(values(s)['Motor2.RunScans'],0)
        tags={t['name']:t for t in s['tags']};self.assertTrue(tags['Motor1.Ready']['address'].startswith('DB101.'));self.assertTrue(tags['Motor2.Ready']['address'].startswith('DB102.'))
    def test_fc_output_and_var_temp_reset_between_calls(self):
        p=simple('FC_Once(Enable := DB1.Flag, Result => DB1.A);')
        p['blocks']=[{'name':'FC_Once','kind':'FC','declarations':'VAR_INPUT\n Enable : BOOL;\nEND_VAR\nVAR_OUTPUT\n Result : DINT;\nEND_VAR\nVAR_TEMP\n Scratch : DINT;\nEND_VAR', 'networks':[{'language':'SCL','source':'Scratch := Scratch + 1; IF Enable THEN Result := Scratch; END_IF;'}]}]
        self.load(compile_project(p));s=self.runtime.command('STATUS');self.write(s,'DB1.Flag',True)
        self.assertEqual(values(self.runtime.command('STEP'))['DB1.A'],1)
        self.assertEqual(values(self.runtime.command('STEP'))['DB1.A'],1)
        self.write(s,'DB1.Flag',False);self.assertEqual(values(self.runtime.command('STEP'))['DB1.A'],0)
    def test_watchdog_fault_is_reported(self):
        p=simple('WHILE TRUE DO DB1.A := DB1.A + 1; END_WHILE;',watchdog_ms=1)
        self.load(compile_project(p));self.runtime.command('RUN');s=wait_for(self.runtime,lambda s:s['state']=='FAULT')
        self.assertIn('watchdog',s['fault'])
    def test_native_raw_db_writes_share_symbolic_memory_and_bits(self):
        p=simple('');p['tasks'][0]['networks']=[{'language':'C','source':'PlcValue v = {0}; v.type=PLC_DINT; v.integer=123456; PLC_TRY(ctx->api->write_db(ctx,1,0,-1,&v)); v.type=PLC_BOOL; v.integer=1; PLC_TRY(ctx->api->write_db(ctx,1,4,0,&v));'}]
        self.load(compile_project(p));s=self.runtime.command('STEP');self.assertEqual(values(s)['DB1.A'],123456);self.assertTrue(values(s)['DB1.Flag']);self.assertTrue(values(s)['DB1.Other'])
    def test_stl_loads_capture_values_at_instruction_time(self):
        p=simple('');p['tasks'][0]['networks']=[{'language':'STL','source':'L 5\nT DB1.A\nL DB1.A\nL 9\nT DB1.A\n+I\nT DB1.A'}]
        self.load(compile_project(p));self.assertEqual(values(self.runtime.command('STEP'))['DB1.A'],14)
    def test_higher_priority_cyclic_ob_can_interrupt_main_at_checkpoints(self):
        p=simple('WHILE NOT DB1.Flag DO\n DB1.A := DB1.A + 1;\nEND_WHILE;',period_ms=100,watchdog_ms=1000)
        # A delayed Windows worker may release OB35 before the first OB1.
        # Only release the loop after OB1 has entered it, proving preemption.
        p['tasks'].append({'ob':35,'kind':'cyclic','period_ms':5,'priority':12,'watchdog_ms':1000,'networks':[{'language':'SCL','source':'IF DB1.A > 0 THEN DB1.Flag := TRUE; END_IF;'}]})
        self.load(compile_project(p));self.runtime.command('RUN');s=wait_for(self.runtime,lambda s:values(s)['DB1.Flag']);self.assertEqual(s['state'],'RUN');self.assertGreater(values(s)['DB1.A'],0)
    def test_runtime_fault_is_contained_and_outputs_go_zero(self):
        p=simple('Motor := TRUE; DB1.A := 1 / DB1.A;');p['globals']=[{'name':'Motor','type':'BOOL','address':'%Q0.0','initial':False}]
        self.load(compile_project(p));self.runtime.command('RUN');s=wait_for(self.runtime,lambda s:s['state']=='FAULT')
        self.assertIn('division by zero',s['fault']);self.assertFalse(values(s)['Motor'])
        with self.assertRaisesRegex(ProjectError,'RESET'):self.runtime.command('RUN')
        self.assertEqual(self.runtime.command('RESET')['state'],'STOP')
    def test_compile_type_error_has_no_load_side_effect(self):
        old=self.load()
        with self.assertRaisesRegex(ProjectError,'type mismatch'):compile_project(simple('DB1.A := TRUE;'))
        self.assertEqual(self.runtime.command('STATUS')['build_id'],old['build_id'])

class HttpTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);path=Path(self.temp.name)/'project.json';path.write_text(json.dumps(DEMO))
        self.studio=Studio(path);self.addCleanup(self.studio.runtime.close)
        self.server=ThreadingHTTPServer(('127.0.0.1',0),handler_for(self.studio));self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.addCleanup(self.server.server_close);self.addCleanup(self.server.shutdown)
        self.url=f'http://127.0.0.1:{self.server.server_address[1]}'
    def test_rejects_missing_token_and_foreign_origin(self):
        for headers in ({},{'X-PLC-Token':self.studio.token,'Origin':'https://example.com'}):
            with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(urllib.request.Request(self.url+'/api/session',headers=headers))
            self.assertEqual(error.exception.code,403)
    def test_authenticated_project_and_static_assets(self):
        request=urllib.request.Request(self.url+'/api/session',headers={'X-PLC-Token':self.studio.token})
        with urllib.request.urlopen(request) as response:self.assertEqual(json.load(response)['project']['name'],DEMO['name'])
        with urllib.request.urlopen(self.url+'/') as response:self.assertIn(b'SOFT-PLC',response.read())

if __name__=='__main__':unittest.main()
