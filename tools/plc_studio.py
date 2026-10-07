"""Loopback-only engineering server. Optional I/O clients run outside the PLC scan thread."""
from __future__ import annotations
import argparse
import atexit
import copy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import queue
import secrets
import subprocess
import threading
import time
import webbrowser
from plc_build import ROOT, BUILD, build_tools, compile_project
from plc_project import prepare, ProjectError
from plc_io import IOManager, validate_links

class RuntimeProcess:
    def __init__(self):
        self.lock=threading.RLock();self.process=None;self.responses=queue.Queue();self.last_fault=''
    def ensure(self, executable):
        with self.lock:
            if self.process and self.process.poll() is None:return
            self.responses=queue.Queue();self.last_fault=''
            self.process=subprocess.Popen([str(executable)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,encoding='utf-8',bufsize=1)
            process=self.process;responses=self.responses
            def reader():
                for line in process.stdout:
                    try:responses.put(json.loads(line))
                    except ValueError:responses.put({'ok':False,'error':'Runtime protocol error: native blocks must not write to stdout'})
                responses.put({'ok':False,'error':'Runtime process exited'})
            threading.Thread(target=reader,daemon=True).start()
    def command(self, command, timeout=3):
        with self.lock:
            if not self.process or self.process.poll() is not None:
                if command=='STATUS':return {'ok':True,'state':'DISCONNECTED' if self.last_fault else 'EMPTY','fault':self.last_fault}
                raise ProjectError('Runtime is not connected. Compile and load a program first.')
            try:
                self.process.stdin.write(command+'\n');self.process.stdin.flush()
                result=self.responses.get(timeout=timeout)
                if not result.get('ok'):raise ProjectError(result.get('error','Runtime error'))
                return result
            except (queue.Empty,BrokenPipeError,OSError):
                self.last_fault='Runtime became unresponsive and was stopped. Check native C/C++ blocks for blocking calls.'
                self.process.kill();self.process.wait(timeout=3);raise ProjectError(self.last_fault)
    def close(self):
        with self.lock:
            if self.process and self.process.poll() is None:
                try:self.command('QUIT',timeout=2);self.process.wait(timeout=2)
                except Exception:self.process.kill();self.process.wait(timeout=3)
            if self.process:
                for stream in (self.process.stdin,self.process.stdout):
                    if stream:stream.close()

class Studio:
    def __init__(self, path):
        self.path=path;self.project=json.loads(path.read_text(encoding='utf-8'));self.token=secrets.token_urlsafe(32)
        self.lock=threading.RLock();self.runtime=RuntimeProcess();self.result=None;self.busy=False;self.build_log='';self.build_error='';self.built_source=None
        self.io=IOManager(self.runtime)
        atexit.register(self.close)
    @staticmethod
    def canonical(project):return json.dumps({k:v for k,v in project.items() if k != 'io_links'},sort_keys=True,separators=(',',':'))
    def save(self, project):
        prepare(project)
        validate_links(project.get('io_links', []))
        with self.lock:
            self.project=copy.deepcopy(project)
            self.path.parent.mkdir(parents=True,exist_ok=True)
            temp=self.path.with_suffix('.tmp');temp.write_text(json.dumps(project,indent=2),encoding='utf-8');temp.replace(self.path)
    def build(self):
        with self.lock:
            if self.busy:raise ProjectError('Build already in progress')
            project=copy.deepcopy(self.project);self.busy=True;self.build_error='';self.build_log='Compiling project…';self.result=None
        def work():
            log=[]
            try:
                result=compile_project(project,log)
                with self.lock:self.result=result;self.built_source=self.canonical(project);self.build_log=result['log']
            except Exception as e:
                with self.lock:self.build_error=str(e);self.build_log='\n'.join(log)+'\n'+str(e)
            finally:
                with self.lock:self.busy=False
        threading.Thread(target=work,daemon=True).start()
    def status(self):
        try:snapshot=self.runtime.command('STATUS')
        except ProjectError as e:snapshot={'state':'DISCONNECTED','fault':str(e)}
        with self.lock:
            return {'ok':True,'runtime':snapshot,'io':self.io.status(),'build':{'busy':self.busy,'error':self.build_error,'log':self.build_log,'ready':bool(self.result),'source_matches':bool(self.result and self.built_source==self.canonical(self.project)),'manifest':self.result['manifest'] if self.result else None}}
    def action(self, action, data):
        if action=='project':self.save(data['project']);return {'ok':True}
        if action=='build':self.build();return {'ok':True}
        if action=='load':
            with self.lock:
                if not self.result or self.busy:raise ProjectError('Build a program successfully first')
                if self.built_source!=self.canonical(self.project):raise ProjectError('Project changed since compilation. Build again before loading.')
                result=self.result
            if self.runtime.command('STATUS').get('state')=='RUN':raise ProjectError('STOP before loading a program')
            with self.lock:
                self.io.close()
                self.runtime.ensure(result['runtime']);return self.runtime.command('LOAD\t'+result['module'])
        if action in ('run','stop','step','reset'):
            with self.lock:return self.runtime.command(action.upper())
        if action=='io_apply':
            with self.lock:self.io.apply(self.project.get('io_links', []), self.runtime.command('STATUS'))
            return {'ok':True}
        if action=='io_disconnect':
            with self.lock:
                if self.runtime.command('STATUS').get('state')=='RUN':raise ProjectError('STOP before disconnecting I/O')
                self.io.close()
            return {'ok':True}
        if action=='write':
            ident=data.get('id');value=data.get('value')
            if isinstance(ident,bool) or not isinstance(ident,int) or not 0<=ident<10000:raise ProjectError('Invalid tag ID')
            if isinstance(value,bool):value=int(value)
            if not isinstance(value,(str,int,float)) or not str(value) or any(c.isspace() for c in str(value)):raise ProjectError('Enter one numeric value')
            return self.runtime.command(f'SET\t{ident}\t{value}')
        raise ProjectError('Unknown action')

    def close(self):
        try:
            if self.runtime.process and self.runtime.process.poll() is None:self.runtime.command('STOP')
        except ProjectError:pass
        try:self.io.close()
        finally:self.runtime.close()

def handler_for(studio):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def response(self,status,body,content_type='application/json'):
            if content_type=='application/json':body=json.dumps(body,allow_nan=False).encode()
            elif isinstance(body,str):body=body.encode()
            self.send_response(status);self.send_header('Content-Type',content_type+'; charset=utf-8');self.send_header('Content-Length',str(len(body)))
            self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.send_header('Referrer-Policy','no-referrer')
            self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'")
            self.end_headers();self.wfile.write(body)
        def authorized(self):
            host=self.headers.get('Host','');port=self.server.server_address[1]
            if host not in (f'127.0.0.1:{port}',f'localhost:{port}'):return False
            origin=self.headers.get('Origin')
            if origin and origin not in (f'http://127.0.0.1:{port}',f'http://localhost:{port}'):return False
            return secrets.compare_digest(self.headers.get('X-PLC-Token',''),studio.token)
        def do_GET(self):
            if self.path.startswith('/api/'):
                if not self.authorized():return self.response(403,{'ok':False,'error':'Open Studio using the launcher session link'})
                if self.path=='/api/session':return self.response(200,{'ok':True,'project':studio.project})
                if self.path=='/api/status':return self.response(200,studio.status())
                if self.path=='/api/export':return self.response(200,studio.project)
                return self.response(404,{'ok':False,'error':'Not found'})
            files={'/':('index.html','text/html'),'/app.js':('app.js','text/javascript'),'/style.css':('style.css','text/css')}
            if self.path not in files:return self.response(404,{'ok':False,'error':'Not found'})
            name,typ=files[self.path];return self.response(200,(ROOT/'tools/studio'/name).read_bytes(),typ)
        def do_POST(self):
            if not self.authorized():return self.response(403,{'ok':False,'error':'Invalid session or origin'})
            try:
                length=int(self.headers.get('Content-Length','0'))
                if not 0<length<=1100000:raise ProjectError('Request size must be from 1 byte to 1.1 MB')
                data=json.loads(self.rfile.read(length));action=self.path.removeprefix('/api/')
                if not self.path.startswith('/api/'):raise ProjectError('Invalid route')
                result=studio.action(action,data);self.response(200,result)
            except (ProjectError,ValueError,KeyError,TypeError) as e:self.response(400,{'ok':False,'error':str(e)})
            except Exception as e:self.response(500,{'ok':False,'error':str(e)})
    return Handler

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--project',type=Path);parser.add_argument('--port',type=int,default=0);parser.add_argument('--no-browser',action='store_true');args=parser.parse_args()
    path=args.project or BUILD/'workspace/project.json'
    if not path.exists():path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes((ROOT/'examples/windows_demo/project.json').read_bytes())
    studio=Studio(path);server=ThreadingHTTPServer(('127.0.0.1',args.port),handler_for(studio));url=f'http://127.0.0.1:{server.server_address[1]}/#{studio.token}'
    print('SOFT-PLC Studio\n'+url+'\nKeep this window open. Ctrl+C stops the runtime.',flush=True)
    if not args.no_browser:webbrowser.open(url)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close();studio.close()
if __name__=='__main__':main()
