# SOFT-PLC

## Windows-first PLC Studio

An open-source **compile → load → RUN → monitor** workflow.
Download the **SOFT-PLC-Studio-Windows-Portable** Actions artifact, extract
the ZIPs, and double-click **Start-Studio.cmd** on Windows 10/11 x64.
Python and portable GCC/MinGW-w64 are included: no Visual Studio or separate
Python installation is required. The interface uses your existing browser.
The compiler is unpacked on the first Compile and runs only during builds.

SOFT-PLC uses the MIT license. Bundled tools retain their open-source
licenses; see [component licenses and source links](THIRD-PARTY-NOTICES.md).

- OB1 main task, startup OBs and periodic priority OBs such as OB35
- Numbered DB addresses and separate FB instance DBs
- FB/FC calls in ordered networks
- Graphical Ladder, STL and SCL subsets, plus native C17/C++20 networks
- Compiled DLL loading, STOP/RUN/FAULT, live tag writes and network monitoring
- TCP, UDP and gRPC I/O handshakes; Modbus TCP and serial RTU
- Simultaneous clients and servers, multiple peers and per-input writer ownership
- I/O tag mapping, connection diagnostics and communication failure policies

See **[Windows setup, architecture and supported language scope](docs/windows-studio.md)**.
Start with [`examples/windows_demo/project.json`](examples/windows_demo/project.json).
This is a custom soft PLC inspired by Siemens block organization, not a
Siemens binary-compatible or hard-real-time PLC. See **[I/O setup, Modbus
register mapping and handshake protocol](docs/IO.md)** for external devices.

## Original interpreted runtime

A software PLC (Programmable Logic Controller) runtime, written in C++20.

The long-term goal is to support all four IEC 61131-3 programming languages
(Ladder Logic, Structured Text, Function Block Diagram, Instruction List).
**Phase 1** (this repository's current state) builds the foundational
runtime: a scan-cycle engine, a thread-safe tag/memory database, an I/O
abstraction, a minimal but real Structured Text (ST) execution pipeline, and
opt-in real-time OS scheduling (priority/affinity/memory locking). See
`docs/architecture.md` for the design — including what "real-time" does and
doesn't mean here — and `docs/roadmap.md` for what's next.

## Building

Requires a C++20 compiler and CMake >= 3.20. Unit tests use GoogleTest,
fetched automatically via CMake `FetchContent` (requires network access on
first configure).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Add `-DSOFTPLC_ENABLE_SANITIZERS=ON` to the configure step to build with
AddressSanitizer/UndefinedBehaviorSanitizer. Pass `-DSOFTPLC_BUILD_TESTS=OFF`
to skip building tests (e.g. if network access for FetchContent is
unavailable).

## Running the blink example

```sh
./build/apps/plc_runner/plc_runner examples/blink/blink.st
```

See `examples/blink/README.md` for details, including the optional
`--rt-priority=N` / `--rt-affinity=N` / `--lock-memory` flags.

## Layout

```
include/softplc/   public headers (core/, tags/, io/, st/)
src/                implementation, built into libsoftplc-core
apps/plc_runner/    thin executable: loads an .st file, runs the scan engine
examples/           example ST programs
tests/               GoogleTest unit tests, mirroring src/
docs/               architecture.md, roadmap.md
```
