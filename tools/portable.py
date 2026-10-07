"""Pinned, application-local open-source tools. Normal Studio use is offline."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PORTABLE = ROOT / 'portable'
COMPILER_FILE = 'w64devkit-x64-2.10.0.7z.exe'
COMPILER_URL = 'https://github.com/skeeto/w64devkit/releases/download/v2.10.0/' + COMPILER_FILE
COMPILER_SHA256 = '18d0a4c71a166f8401ab6305781bec5882b40b5e06ba9807c61cb5f3b3c6325e'
PYTHON_URL = 'https://www.python.org/ftp/python/3.13.16/python-3.13.16-embeddable-amd64.zip'
PYTHON_SHA256 = '589dc1e9d02549ca5f680710307ff9b77f6e3ffc4aa3efccd7fa54eeeafac94b'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, target, expected):
    if target.is_file() and sha256(target) == expected:
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_suffix(target.suffix + '.download')
    print('Downloading ' + target.name, flush=True)
    try:
        request = urllib.request.Request(url, headers={'User-Agent': 'SOFT-PLC-portable-builder'})
        with urllib.request.urlopen(request, timeout=60) as response, partial.open('wb') as out:
            shutil.copyfileobj(response, out)
        if sha256(partial) != expected:
            raise RuntimeError('Checksum mismatch: ' + target.name)
        partial.replace(target)
    finally:
        partial.unlink(missing_ok=True)
    return target


def ensure_compiler():
    """Extract the bundled, verified compiler once, without a network request."""
    folder = PORTABLE / 'w64devkit'
    if (folder / 'bin/g++.exe').is_file():
        return folder
    archive = PORTABLE / COMPILER_FILE
    if not archive.is_file():
        return None
    if os.name != 'nt':
        raise RuntimeError('The bundled compiler runs on Windows x64')
    if sha256(archive) != COMPILER_SHA256:
        raise RuntimeError('Bundled compiler checksum mismatch; extract a fresh Portable package')
    # The upstream archive is an ordinary 7-Zip self-extractor. Extract into a
    # temporary directory first so cancellation does not leave a partial kit.
    with tempfile.TemporaryDirectory(prefix='unpack-', dir=PORTABLE) as temporary:
        result = subprocess.run([str(archive), '-y', '-o' + temporary],
                                capture_output=True, text=True, errors='replace', timeout=180)
        candidate = Path(temporary) / 'w64devkit'
        if result.returncode or not (candidate / 'bin/g++.exe').is_file():
            raise RuntimeError('Cannot unpack the bundled compiler: ' + result.stdout + result.stderr)
        if folder.exists():
            raise RuntimeError('Incomplete portable/w64devkit folder; remove it and compile again')
        candidate.replace(folder)
    return folder


def prepare():
    if os.name != 'nt':
        raise RuntimeError('Prepare the Windows Portable package on Windows')
    PORTABLE.mkdir(exist_ok=True)
    download(COMPILER_URL, PORTABLE / COMPILER_FILE, COMPILER_SHA256)
    archive = download(PYTHON_URL, ROOT / 'build/studio/downloads/python-embedded.zip', PYTHON_SHA256)
    target = PORTABLE / 'python'
    target.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        # This is the hash-pinned official archive, with only flat file names.
        for entry in package.infolist():
            if Path(entry.filename).name != entry.filename:
                raise RuntimeError('Unexpected embedded Python archive path')
            (target / entry.filename).write_bytes(package.read(entry))
    # Keep bundled I/O wheels isolated from all system Python installations.
    subprocess.run([sys.executable, '-m', 'pip', 'install', '--only-binary=:all:',
                    '--upgrade', '--target', str(target / 'site-packages'),
                    '-r', str(ROOT / 'tools/requirements-io.txt')], check=True)
    (target / 'python313._pth').write_text('python313.zip\n.\nsite-packages\n../../tools\n', encoding='utf-8')
    subprocess.run([str(target / 'python.exe'), '-c', 'import grpc, serial; from google.protobuf.wrappers_pb2 import BytesValue'], check=True)
    ensure_compiler()
    print('Portable GCC and embedded Python are ready.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--prepare', action='store_true', help='download and verify tools for packaging')
    args = parser.parse_args()
    if args.prepare:
        prepare()
    else:
        parser.error('--prepare is required; Studio extracts bundled tools automatically')
