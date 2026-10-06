# Open-source components

SOFT-PLC's own source is under the MIT license in `LICENSE`.
The engineering interface uses plain HTML, CSS and JavaScript in your existing
browser. The server uses Python's standard library. No Qt, Electron, Node.js
runtime or third-party Python package is required to use Studio.

The **Windows Portable** distribution includes these separate components:

| Component | Version | License and corresponding source |
|---|---|---|
| CPython, embedded Windows x64 distribution | 3.13.16 | PSF license and included component notices in `portable/python/LICENSE.txt`; [release and source downloads](https://www.python.org/downloads/release/python-31316/) |
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
