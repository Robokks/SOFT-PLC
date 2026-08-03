# SOFT-PLC

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
