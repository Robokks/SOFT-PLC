"""Create a Windows source+tools ZIP; compiler SDK and Python remain prerequisites."""
import json
import os
from pathlib import Path
import shutil
import zipfile
from plc_build import ROOT, BUILD, build_tools, compile_project

def main():
    if os.name!='nt':raise SystemExit('Create the Windows package on Windows (or download the CI artifact).')
    compiler,runtime,*_=build_tools()
    demo=compile_project(json.loads((ROOT/'examples/windows_demo/project.json').read_text(encoding='utf-8')))
    output=ROOT/'dist';output.mkdir(exist_ok=True);target=output/'SOFT-PLC-Studio-Windows.zip'
    with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED) as archive:
        for dirname in ('include','src','apps','tools','docs','examples','cmake'):
            for path in (ROOT/dirname).rglob('*'):
                if path.is_file() and '__pycache__' not in path.parts and path.suffix!='.pyc':archive.write(path,Path('SOFT-PLC')/path.relative_to(ROOT))
        for name in ('Start-Studio.cmd','README.md','LICENSE','CMakeLists.txt'):
            archive.write(ROOT/name,Path('SOFT-PLC')/name)
        for path in (compiler,runtime):archive.write(path,Path('SOFT-PLC')/path.relative_to(ROOT))
        for name in ('program.dll','manifest.json','project.json'):
            path=Path(demo['folder'])/name;archive.write(path,Path('SOFT-PLC/prebuilt-demo')/name)
    print(target)
if __name__=='__main__':main()
