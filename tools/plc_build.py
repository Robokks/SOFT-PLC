"""Build the Windows-first runtime and compile a project to a native DLL/module."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid
from plc_project import prepare, map_symbols, ProjectError
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build'/'studio'

def is_msvc(cxx):
    return Path(cxx).name.lower() in ('cl', 'cl.exe')


def mingw_compilers(env):
    """Find native x64 MinGW; a requested standalone compiler never falls back to MSVC."""
    from portable import ensure_compiler
    requested = env.get('SOFTPLC_TOOLCHAIN')
    folder = Path(requested).expanduser() if requested else ensure_compiler()
    if folder:
        binary = folder if folder.name.lower() == 'bin' else folder / 'bin'
        cxx, cc = binary / 'g++.exe', binary / 'gcc.exe'
        if not cxx.is_file() or not cc.is_file():
            raise ProjectError('SOFTPLC_TOOLCHAIN must contain bin/g++.exe and bin/gcc.exe')
        env['PATH'] = str(binary) + os.pathsep + env.get('PATH', '')
    else:
        cxx = shutil.which('g++', path=env.get('PATH'))
        cc = shutil.which('gcc', path=env.get('PATH'))
        if not cxx or not cc:
            return None
    target = subprocess.check_output([str(cxx), '-dumpmachine'], env=env, text=True, timeout=10).strip()
    if target != 'x86_64-w64-mingw32':
        raise ProjectError('A native x64 MinGW compiler is required; found ' + target)
    return env, str(cxx), str(cc)


def compiler_environment():
    env = os.environ.copy()
    if os.name != 'nt':
        cxx = shutil.which('g++', path=env.get('PATH'))
        cc = shutil.which('gcc', path=env.get('PATH'))
        if not cxx or not cc:
            raise ProjectError('g++ and gcc are required')
        return env, cxx, cc
    choice = env.get('SOFTPLC_COMPILER', 'auto').lower()
    if choice not in ('auto', 'mingw', 'msvc'):
        raise ProjectError('SOFTPLC_COMPILER must be auto, mingw or msvc')
    if choice != 'msvc':
        result = mingw_compilers(env)
        if result:
            return result
        if choice == 'mingw' or env.get('SOFTPLC_TOOLCHAIN'):
            raise ProjectError('Use the Windows Portable package, or set SOFTPLC_TOOLCHAIN to an extracted x64 w64devkit folder.')
    if not shutil.which('cl', path=env.get('PATH')):
        vswhere = Path(env.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        if vswhere.exists():
            install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
            if install:
                vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
                with tempfile.TemporaryDirectory(prefix='softplc-env-') as temp:
                    batch = Path(temp) / 'environment.cmd'
                    batch.write_text('@echo off\ncall "' + str(vcvars) + '" >nul\nif errorlevel 1 exit /b 1\nset\n')
                    result = subprocess.run([env.get('COMSPEC', 'cmd.exe'), '/d', '/c', str(batch)], capture_output=True, text=True)
                if result.returncode:
                    raise ProjectError('MSVC environment setup failed: ' + result.stderr.strip())
                for line in result.stdout.splitlines():
                    if '=' in line and not line.startswith('='):
                        key, value = line.split('=', 1)
                        env[key.upper()] = value
    cxx = shutil.which('cl', path=env.get('PATH'))
    if not cxx:
        raise ProjectError('No compiler found. Download the Windows Portable package (includes GCC), or set SOFTPLC_TOOLCHAIN to an extracted x64 w64devkit folder.')
    return env, cxx, cxx


def native_link_flags(cxx):
    # The PLC tools and DLLs must run with only Windows DLLs on PATH.
    return ['-static', '-static-libgcc', '-static-libstdc++', '-s'] if os.name == 'nt' and not is_msvc(cxx) else []

def run(args,env,cwd,log):
    result=subprocess.run([str(a) for a in args],cwd=cwd,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=180)
    output=result.stdout+result.stderr
    if output:log.append(output)
    if result.returncode:raise ProjectError(output or f'Compiler exited {result.returncode}')

def build_tools(log=None):
    log=[] if log is None else log;env,cxx,cc=compiler_environment();kind='msvc' if is_msvc(cxx) else 'mingw' if os.name=='nt' else 'gcc';folder=BUILD/'tools'/kind;folder.mkdir(parents=True,exist_ok=True)
    exe='.exe' if os.name=='nt' else ''
    common=[ROOT/'src/tags/value.cpp',ROOT/'src/tags/tag_store.cpp']
    compiler=folder/('plc_codegen'+exe);runtime=folder/('plc_runtime'+exe)
    sources={compiler:common+[ROOT/f'src/st/{n}.cpp' for n in ('lexer','parser','pou_binder','interpreter','st_program')]+[ROOT/'apps/plc_codegen/main.cpp'],runtime:common+[ROOT/'apps/plc_runtime/main.cpp']}
    headers=list((ROOT/'include').rglob('*.h'))+list((ROOT/'include').rglob('*.hpp'))
    stamp=folder/'compiler.json'
    identity={'compiler':str(Path(cxx).resolve()),'mtime':Path(cxx).stat().st_mtime_ns}
    same_compiler=stamp.exists() and json.loads(stamp.read_text(encoding='utf-8'))==identity
    log.append('Compiler: '+str(cxx)+'\n')
    for target,files in sources.items():
        newest=max(p.stat().st_mtime for p in files+headers+[Path(__file__)])
        if same_compiler and target.exists() and target.stat().st_mtime>=newest:continue
        if is_msvc(cxx):
            command=[cxx,'/nologo','/std:c++20','/EHsc','/utf-8','/O2',f'/I{ROOT/"include"}',*files,f'/Fe:{target}']
        else:command=[cxx,'-std=c++20','-O1','-pthread',f'-I{ROOT/"include"}',*files,'-o',target]+(['-ldl'] if target==runtime and os.name!='nt' else [])+native_link_flags(cxx)
        run(command,env,folder,log)
    stamp.write_text(json.dumps(identity),encoding='utf-8')
    return compiler,runtime,env,cxx,cc

def compile_project(project,log=None):
    log=[] if log is None else log
    prepared=prepare(project)
    compiler,runtime,env,cxx,cc=build_tools(log)
    folder=BUILD/'programs'/(prepared.build_id+'-'+uuid.uuid4().hex[:8]);folder.mkdir(parents=True)
    (folder/'project.json').write_text(json.dumps(prepared.project,indent=2),encoding='utf-8')
    (folder/'program.st').write_text(prepared.source,encoding='utf-8')
    run([compiler,folder/'program.st',folder/'generated.cpp',folder/'symbols.json'],env,folder,log)
    symbols=map_symbols(prepared,json.loads((folder/'symbols.json').read_text(encoding='utf-8')))
    dispatch=next(s['id'] for s in symbols if s['name']=='__plc_dispatch')
    native_objects=[]
    for native in prepared.native:
        is_c=native['language']=='C';path=folder/(native['name']+('.c' if is_c else '.cpp'));obj=path.with_suffix('.obj' if os.name=='nt' else '.o')
        source='#include "softplc/runtime/program_abi.h"\n'+('' if is_c else 'extern "C" ')+f'int32_t {native["name"]}(PlcContext* ctx) {{\n'+native['source']+'\nreturn 0;\n}\n'
        path.write_text(source,encoding='utf-8')
        if is_msvc(cxx):command=[cxx,'/nologo','/TC' if is_c else '/TP','/std:c17' if is_c else '/std:c++20','/EHsc','/utf-8','/O2',f'/I{ROOT/"include"}','/c',path,f'/Fo:{obj}']
        else:command=[cc if is_c else cxx,'-std=c17' if is_c else '-std=c++20',*(['-fPIC'] if os.name!='nt' else []),'-O1',f'-I{ROOT/"include"}','-c',path,'-o',obj]
        run(command,env,folder,log);native_objects.append(obj)
    # All strings below are JSON/C++ string literals, never shell fragments.
    quote=lambda s:json.dumps(s,ensure_ascii=False)
    text=['#include "generated.cpp"']
    for task in prepared.project['tasks']:
        ob=task['ob'];text.append(f'''static int32_t run_ob{ob}(PlcContext* c) {{
try {{ write(c,{dispatch},TypeId::DInt,Value(int32_t({ob}))); generated_run(c); return 0; }}
catch(const Cancelled&) {{return -2;}}
catch(const std::exception& e) {{c->api->fault(c,e.what());return -1;}}
catch(...) {{c->api->fault(c,"native block exception");return -1;}}
}}''')
    text.append('static const PlcTagDef program_tags[]={')
    for s in symbols:text.append('{'+f'{quote(s["name"])},{s["type"]},generated_initials[{s["id"]}],{s["area"]},{s["db"]},{s["offset"]},{s["bit"]},{quote(s["address"])}'+'},')
    text.append('};\nstatic const PlcTaskDef program_tasks[]={')
    for t in prepared.project['tasks']:
        kind={'startup':0,'main':1,'cyclic':2}[t['kind']]
        text.append('{'+f'{quote(t["name"])},{t["ob"]},{kind},{t.get("period_ms",10)*1000}ULL,{t.get("priority",1 if t["kind"]=="main" else 12)},{t.get("watchdog_ms",1000)*1000}ULL,&run_ob{t["ob"]}'+'},')
    text.append('};\nstatic const PlcNetworkDef program_networks[]={')
    for n in prepared.networks:text.append('{'+f'{n["id"]},{quote(n["block"])},{quote(n["title"])},{quote(n["language"])}'+'},')
    if not prepared.networks:text.append('{0,"","",""},')
    text.append('};\nstatic const PlcProgramV1 program={'+f'PLC_ABI_VERSION,sizeof(PlcProgramV1),{quote(prepared.project["name"])},{quote(prepared.build_id)},{len(symbols)},program_tags,{len(prepared.project["tasks"])},program_tasks,{len(prepared.networks)},program_networks'+'};')
    text.append('extern "C" PLC_EXPORT const PlcProgramV1* softplc_program_v1(){return &program;}')
    (folder/'module.cpp').write_text('\n'.join(text),encoding='utf-8')
    module=folder/('program.dll' if os.name=='nt' else 'program.so')
    if is_msvc(cxx):command=[cxx,'/nologo','/std:c++20','/EHsc','/utf-8','/O2','/LD',f'/I{ROOT/"include"}',folder/'module.cpp',ROOT/'src/tags/value.cpp',*native_objects,f'/Fe:{module}']
    else:command=[cxx,'-std=c++20','-O1',*(['-fPIC'] if os.name!='nt' else []),'-shared',f'-I{ROOT/"include"}',folder/'module.cpp',ROOT/'src/tags/value.cpp',*native_objects,'-o',module]+native_link_flags(cxx)
    run(command,env,folder,log)
    manifest={'schema':1,'name':prepared.project['name'],'build_id':prepared.build_id,'platform':sys.platform,'module':module.name,'symbols':symbols,'networks':prepared.networks,'tasks':prepared.project['tasks']}
    (folder/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    log.append(f'Build succeeded: {module}\n')
    return {'module':str(module),'runtime':str(runtime),'manifest':manifest,'log':'\n'.join(log),'folder':str(folder)}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('project',nargs='?');parser.add_argument('--tools-only',action='store_true');args=parser.parse_args();log=[]
    try:
        if args.tools_only:build_tools(log)
        elif args.project:
            result=compile_project(json.loads(Path(args.project).read_text(encoding='utf-8')),log);print(result['module'])
        else:parser.error('project path or --tools-only required')
        print('\n'.join(log))
    except (ProjectError,ValueError,KeyError,subprocess.SubprocessError) as e:print(f'Build failed: {e}',file=sys.stderr);return 1
    return 0
if __name__=='__main__':raise SystemExit(main())
