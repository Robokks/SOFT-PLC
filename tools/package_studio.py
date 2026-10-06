"""Create a source/tools ZIP, optionally with offline Python and GCC payloads."""
import argparse
import json
import os
from pathlib import Path
import zipfile
from plc_build import ROOT, build_tools, compile_project, is_msvc
from portable import PORTABLE, COMPILER_FILE, COMPILER_SHA256, sha256


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--portable', action='store_true')
    args = parser.parse_args()
    if os.name != 'nt':
        raise SystemExit('Create the Windows package on Windows (or download the CI artifact).')
    if args.portable:
        if not (PORTABLE / 'python/python.exe').is_file():
            raise SystemExit('Run python tools/portable.py --prepare first')
        if sha256(PORTABLE / COMPILER_FILE) != COMPILER_SHA256:
            raise SystemExit('Portable compiler payload checksum mismatch')
        os.environ['SOFTPLC_COMPILER'] = 'mingw'
        os.environ['SOFTPLC_TOOLCHAIN'] = str(PORTABLE / 'w64devkit')
    compiler, runtime, _, cxx, _ = build_tools()
    demo = compile_project(json.loads((ROOT / 'examples/windows_demo/project.json').read_text(encoding='utf-8')))
    output = ROOT / 'dist'
    output.mkdir(exist_ok=True)
    suffix = '-Portable' if args.portable else ''
    target = output / ('SOFT-PLC-Studio-Windows' + suffix + '.zip')
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        for dirname in ('include', 'src', 'apps', 'tools', 'docs', 'examples', 'cmake', 'tests'):
            for path in (ROOT / dirname).rglob('*'):
                if path.is_file() and '__pycache__' not in path.parts and path.suffix != '.pyc':
                    archive.write(path, Path('SOFT-PLC') / path.relative_to(ROOT))
        for name in ('Start-Studio.cmd', 'README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'CMakeLists.txt'):
            archive.write(ROOT / name, Path('SOFT-PLC') / name)
        for path in (compiler, runtime):
            archive.write(path, Path('SOFT-PLC') / path.relative_to(ROOT))
        for name in ('program.dll', 'manifest.json', 'project.json'):
            path = Path(demo['folder']) / name
            archive.write(path, Path('SOFT-PLC/prebuilt-demo') / name)
        if args.portable:
            # Retain the upstream compressed payload to keep the download small.
            archive.write(PORTABLE / COMPILER_FILE, Path('SOFT-PLC/portable') / COMPILER_FILE,
                          compress_type=zipfile.ZIP_STORED)
            for path in (PORTABLE / 'python').rglob('*'):
                if path.is_file():
                    archive.write(path, Path('SOFT-PLC') / path.relative_to(ROOT))
            notice = PORTABLE / 'w64devkit/COPYING.MinGW-w64-runtime.txt'
            archive.write(notice, 'SOFT-PLC/COPYING.MinGW-w64-runtime.txt')
            archive.writestr('SOFT-PLC/START-HERE.txt',
                            'Extract this whole folder, then double-click Start-Studio.cmd.\n'
                            'Python and the open-source GCC compiler are included.\n'
                            'No Visual Studio, administrator access, or installation is needed.\n'
                            'The first Compile unpacks the compiler and builds the tools locally.\n'
                            'Keep the console open while using Studio.\n'
                            'Choose Compile, Load to CPU, RUN, then Online watch.\n'
                            'Source and component licenses: THIRD-PARTY-NOTICES.md.\n')
    print(target)
    print(f'Package: {target.stat().st_size / 1024 / 1024:.1f} MiB; '
          f'native runtime: {runtime.stat().st_size / 1024 / 1024:.2f} MiB; '
          f'compiler: {"MSVC" if is_msvc(cxx) else "GCC / MinGW-w64"}')


if __name__ == '__main__':
    main()
