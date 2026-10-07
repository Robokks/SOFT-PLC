# Open-source components

SOFT-PLC's own source is under the MIT license in `LICENSE`.
The engineering interface uses plain HTML, CSS and JavaScript in your existing
browser. The engineering server uses Python's standard library. No Qt, Electron or
Node.js runtime is required. Optional gRPC and serial I/O use the open-source
Python packages below, bundled in the Portable distribution.

The **Windows Portable** distribution includes these separate components:

| Component | Version | License and corresponding source |
|---|---|---|
| CPython, embedded Windows x64 distribution | 3.13.16 | PSF license and included component notices in `portable/python/LICENSE.txt`; [release and source downloads](https://www.python.org/downloads/release/python-31316/) |
| grpcio | 1.84.0 | Apache-2.0; [source](https://github.com/grpc/grpc/tree/v1.84.0); bundled wheel includes license and third-party notices |
| protobuf | 7.36.2 | BSD-3-Clause; [source releases](https://github.com/protocolbuffers/protobuf/releases); bundled wheel includes license |
| pyserial | 3.5 | BSD-3-Clause; [source](https://github.com/pyserial/pyserial/tree/v3.5); bundled wheel includes license |
| typing_extensions | 4.16.0 | PSF-2.0; [source](https://github.com/python/typing_extensions); bundled wheel includes license |
| w64devkit, unmodified x64 self-extracting archive | 2.10.0 | Component-specific open-source licenses inside the archive; [build recipes and patches](https://github.com/skeeto/w64devkit/tree/v2.10.0); [complete upstream source archive](https://github.com/skeeto/w64devkit/releases/download/v2.10.0/source.tar) |

w64devkit contains GCC, GNU binutils, MinGW-w64 and development utilities.
The compiler tools include GPL-licensed components; they are separate programs
invoked by Studio. Their licenses apply independently of SOFT-PLC's MIT license.
The GCC Runtime Library Exception applies to the covered GCC runtime libraries
linked into generated programs. The compiler's license texts are retained in
its original archive and are available under `portable/w64devkit` after the
first compilation.

`COPYING.MinGW-w64-runtime.txt` accompanies the GCC-built binaries in the
portable ZIP. Preserve it when redistributing those binaries. The complete
corresponding compiler source is available at the source-archive link above,
separately from the Studio download. Exact download URLs and SHA-256 checksums
are pinned in `tools/portable.py`.

Python package license files are retained inside their `*.dist-info` directories
under `portable/python/site-packages`. Pinned versions are in
`tools/requirements-io.txt`; binary wheels are installed during packaging.
