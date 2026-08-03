# Architecture — Phase 1

SOFT-PLC is a software PLC (Programmable Logic Controller) runtime written in
C++20. Phase 1 builds the foundational runtime: a scan-cycle engine, a tag
(variable) database, an I/O abstraction, and a minimal but real Structured
Text (ST) execution pipeline. There is no GUI/HMI/web UI in this phase.

## Layout

A single static library, `libsoftplc-core`, contains everything; `plc_runner`
is a thin executable that links against it.

```
include/softplc/
  core/   scan_engine.hpp, program.hpp, native_program.hpp
  tags/   value.hpp, tag.hpp, tag_store.hpp
  io/     io_driver.hpp, simulated_io_driver.hpp
  st/     token.hpp, lexer.hpp, ast.hpp, parser.hpp, interpreter.hpp, st_program.hpp
src/                        (matching .cpp files)
apps/plc_runner/            main.cpp — loads an .st file and runs the engine
examples/blink/             a minimal ST program toggling a direct-addressed output
tests/                      GoogleTest unit tests, mirroring src/
```

## Scan-cycle engine (`core/`)

`ScanEngine` runs a fixed-cycle-time loop on a `std::jthread`, in strict
order: **read inputs → execute program → write outputs → housekeeping**,
timed with `std::chrono::steady_clock`. `runOnce()` performs exactly one scan
synchronously and is exposed separately from the threaded `start()`/`stop()`
API so tests can drive deterministic scans without relying on real timing.

Overruns (a scan taking longer than the configured cycle time) are recorded
in `ScanDiagnostics` (`cycleCount`, `overrunCount`, `lastCycleDuration`,
`maxCycleDuration`) rather than treated as fatal.

`IProgram` (in `core/program.hpp`) is the abstraction a scan executes each
cycle — named after the IEC 61131-3 POU (Program Organization Unit) concept
so Ladder/FBD/IL implementations can share the same interface in later
phases. Two implementations ship in Phase 1: `st::StProgram` (the real ST
pipeline) and `core::NativeProgram` (wraps an arbitrary C++ callable; used
for engine tests and bootstrapping without the ST parser).

## Tag database (`tags/`)

`TagStore` is a thread-safe symbol table of `Tag`s, guarded by a
`std::shared_mutex` (concurrent readers, exclusive writer) so it is ready for
future concurrent consumers (OPC-UA/Modbus/HMI) reading tags while the scan
thread runs. Every tag has a symbolic name; some additionally carry an IEC
direct address (`%I`/`%Q`/`%M`, byte + bit offset) usable interchangeably
with the name (e.g. `AT %Q0.0` in an ST `VAR` declaration).

`Value` is a `std::variant` covering the Phase 1 elementary type set: `BOOL,
BYTE, INT, DINT, REAL, LREAL, TIME, STRING`. `TIME` is represented as
`std::chrono::milliseconds`.

Hot-path `read()`/`write()` take a `TagId` (a plain vector index) resolved
once at program-load time — the ST interpreter never does a string lookup
during a scan.

## I/O abstraction (`io/`)

`IIoDriver` has two methods, `readInputs()`/`writeOutputs()`, called by
`ScanEngine` around program execution. Phase 1 ships only
`SimulatedIoDriver` (settable simulated inputs, recorded outputs) — no real
hardware yet — but the interface is shaped so a future Modbus/GPIO driver
needs no engine changes.

## Structured Text (`st/`)

A real pipeline — lexer → recursive-descent parser → AST → tree-walking
interpreter — rather than a native-callback shortcut, so ST source can
actually declare tags (`VAR` blocks auto-populate the `TagStore`) instead of
requiring hand-written C++ tag declarations for every program.

Minimal Phase 1 subset:
- `PROGRAM <name> ... END_PROGRAM` wrapper
- `VAR` / `VAR_INPUT` / `VAR_OUTPUT` declarations, optionally `AT <address>`,
  with types `BOOL, BYTE, INT, DINT, REAL, LREAL, TIME, STRING` and an
  optional literal initializer
- Literals: integer, real, `TRUE`/`FALSE`, `T#...` time, `'...'` string,
  `%I`/`%Q`/`%M` direct addresses
- Assignment (`x := expr;`)
- Expressions: `+ - * /`, comparisons (`= <> < > <= >=`), boolean
  (`AND OR NOT XOR`), parentheses, unary minus — standard precedence, lowest
  to highest binding: `OR, XOR, AND, = <>, < > <= >=, + -, * /, NOT/unary -`
- Control flow: `IF/ELSIF/ELSE/END_IF`, `WHILE/DO/END_WHILE` (loop bodies are
  capped at 1,000,000 iterations per scan as a runaway-loop guard)

`StProgram::load()` parses the source, declares each `VAR` entry into the
`TagStore`, and resolves every identifier reference to a `TagId` — all before
the first scan, so `Interpreter::run()` never performs name lookups. See
`docs/roadmap.md` for what's explicitly out of scope for Phase 1.

## Testing

Each layer is tested in isolation (`tests/tags`, `tests/core`, `tests/io`,
`tests/st/{lexer,parser,interpreter}_test.cpp`) using GoogleTest, fetched via
CMake `FetchContent`. `ScanEngine` is tested primarily through `runOnce()`
for determinism; one test exercises the real threaded `start()`/`stop()`
loop. `SOFTPLC_ENABLE_SANITIZERS` (CMake option) turns on
AddressSanitizer/UndefinedBehaviorSanitizer for dev/CI builds.
