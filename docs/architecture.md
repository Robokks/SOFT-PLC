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
  core/   scan_engine.hpp, program.hpp, native_program.hpp, rt_scheduling.hpp
  tags/   value.hpp, tag.hpp, tag_store.hpp
  io/     io_driver.hpp, simulated_io_driver.hpp,
          modbus_mapping.hpp, modbus_client.hpp, modbus_tcp_driver.hpp
  net/    tcp_socket.hpp, tcp_listener.hpp
  st/     token.hpp, lexer.hpp, ast.hpp, parser.hpp, interpreter.hpp, st_program.hpp
src/                        (matching .cpp files)
apps/plc_runner/            main.cpp — loads an .st file and runs the engine
examples/blink/             a minimal ST program toggling a direct-addressed output
tests/                      GoogleTest unit tests, mirroring src/, plus
                             tests/support/mock_modbus_server.{hpp,cpp}
```

## Scan-cycle engine (`core/`)

`ScanEngine` runs a fixed-cycle-time loop on a `std::jthread`, in strict
order: **read inputs → execute program → write outputs → housekeeping**,
timed with `std::chrono::steady_clock`. `runOnce()` performs exactly one scan
synchronously and is exposed separately from the threaded `start()`/`stop()`
API so tests can drive deterministic scans without relying on real timing.

Overruns (a scan taking longer than the configured cycle time) are recorded
in `ScanDiagnostics` (`cycleCount`, `overrunCount`, `lastCycleDuration`,
`maxCycleDuration`, `minCycleDuration`) rather than treated as fatal. The
threaded loop additionally tracks `lastWakeJitter`/`maxWakeJitter` (how late
the thread woke relative to its fixed schedule) and `resyncCount` (see
"Real-time characteristics" below).

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
`ScanEngine` around program execution. `SimulatedIoDriver` (settable
simulated inputs, recorded outputs) and `ModbusTcpIoDriver` (see below) are
both drop-in implementations — neither requires any `ScanEngine` change.

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

## Modbus TCP I/O driver (`io/modbus_*`, `net/`)

`ModbusTcpIoDriver` is a Modbus TCP **client (master)**: it polls real (or,
in tests, mock) Modbus TCP servers and feeds `%I`/`%Q` tags. It is *not* a
Modbus server exposing this project's own `TagStore` — that's the separate
"external tag access" item in `docs/roadmap.md`.

**Never blocks the scan thread on network I/O.** A Modbus round-trip is
unbounded-latency network I/O; doing it synchronously inside
`readInputs()`/`writeOutputs()` would directly undermine the real-time work
above — one slow device could stall an entire scan cycle. Instead, each
configured device gets its own dedicated background polling thread (not one
shared poller, so one hung device can't stall polling for every other
device) maintaining its own TCP connection and an in-memory cache of latest
known values. `readInputs()`/`writeOutputs()` only ever do fast,
lock-guarded copies between that cache and `TagStore`.

**Freeze-last-good-value semantics**: a failed poll leaves the cached value
and its validity flag untouched — never silently replaced with garbage,
never a crash. A point that has never successfully polled stays at its tag's
declared default. Each device optionally writes a `healthTagAddress` `BOOL`
tag every `readInputs()` call, so ST logic can interlock on "device offline"
directly rather than only a C++ caller being able to observe it.

**Protocol codec is vendored, not hand-written**: `nanoMODBUS`
(`debevv/nanoMODBUS`, MIT-licensed, fetched via CMake `FetchContent` —
`cmake/FetchNanomodbus.cmake`) implements MBAP/PDU encode/decode/validation.
It was chosen over `libmodbus` (LGPL v3, clashes with this project's MIT/
zero-runtime-dependency posture) and other MIT alternatives that lacked a
portable (non-Linux-only) transport. nanoMODBUS is transport-agnostic — it
takes read/write callbacks rather than owning sockets — so `net::TcpSocket`
(a small, generic, cross-platform client socket; first real
`#ifdef`-gated POSIX-vs-Winsock2 code in this codebase) still supplies the
actual transport. `net::TcpListener` is the equally generic server-side
counterpart, used only by the test-only mock server (production Modbus TCP
here is client-only and never binds/listens).

Two things this project still owns on top of nanoMODBUS: **word order**
across multi-register values (`DINT`/`REAL` spanning 2 registers is not
Modbus-spec-defined and varies by device vendor — a per-point `WordOrder`
config knob) and **error translation** (nanoMODBUS's `nmbs_error` mapped
into this project's structured, never-throwing `RequestResult`, mirroring
`core::rt::RtApplyResult`'s convention).

**Tag-to-register mapping** (`modbus_mapping.hpp`): a `ModbusPointMapping`
binds one IEC tag address to one Modbus register/coil. v1 deliberately
restricts pairings (enforced by `validateDeviceConfig()`, which throws
`std::invalid_argument` at construction time, matching
`TagStore::declare()`'s throw-at-setup convention): `Coil`/`DiscreteInput`
only pair with `BOOL`; `HoldingRegister`/`InputRegister` only pair with
`BYTE`/`INT`/`DINT`/`REAL`. `buildBatches()` coalesces strictly-contiguous
points per register type into one request (capped at the Modbus protocol
limits), used for both reads and writes.

Like `SimulatedIoDriver`, this driver never declares tags itself — points
are matched against tags already declared elsewhere (typically an ST
program's `VAR ... AT %I0.0 ...`), looked up by address each call.

**Known limitation, deliberately not fixed yet**: no gap-tolerant batching
(a single-register gap between two points always starts a new request, even
though bridging small gaps could reduce request count), no config-file
format (devices are configured via a C++ `ModbusDeviceConfig`/
`ModbusPointMapping` API only — wiring a config file into `apps/plc_runner`
is future work), and Modbus RTU (serial) is a separate, not-yet-built driver.

## Real-time characteristics

Out of the box, on a stock (non-PREEMPT_RT) Linux or Windows kernel, the scan
engine is **soft real-time**: it holds a fixed cadence and the scan-cycle hot
path (`TagStore::read`/`write`, `Interpreter::evaluate`/`execStmt`) performs
no heap allocation for any tag type except `STRING` (avoid `STRING` tags in
timing-critical logic), but the OS scheduler can still preempt the scan
thread for milliseconds under load, and a `WHILE` loop is capped at
1,000,000 iterations per scan rather than having a proven worst-case
execution time. Occasional missed deadlines are possible; nothing here
guarantees they can't happen.

`core/rt_scheduling.hpp` (`softplc::core::rt`) adds opt-in OS-level
scheduling, applied to the scan thread itself via
`ScanEngine::setRtPolicy()` (call before `start()`):
- `enableRealtimePriority` — `SCHED_FIFO` on Linux (priority clamped into
  `sched_get_priority_min/max`), `THREAD_PRIORITY_TIME_CRITICAL` on Windows.
- `cpuAffinity` — pins the scan thread to one logical CPU.
- `lockMemory` — `mlockall(MCL_CURRENT | MCL_FUTURE)` on Linux (no
  process-wide equivalent on Windows, so it's a documented no-op there);
  paired with a small fixed-size `prefaultStack()` call so an early stack
  page fault doesn't itself add latency after memory is locked.

Every one of these is best-effort: missing privileges (no `CAP_SYS_NICE`/
root on Linux, a container's default `RLIMIT_MEMLOCK`) degrade to a warning
in `ScanEngine::rtApplyResult()` rather than a crash or thrown exception.
`ScanEngine::start()` blocks until the scan thread has actually applied the
policy to itself, so `rtApplyResult()` is race-free and accurate as soon as
`start()` returns. `plc_runner` exposes this via `--rt-priority=N`,
`--rt-affinity=N`, and `--lock-memory` (all off by default).

Raising `RLIMIT_MEMLOCK` itself needs root/`CAP_SYS_RESOURCE` and can't be
done reliably from inside the process — it's a deployment-time setting
(systemd `LimitMEMLOCK=infinity`, `/etc/security/limits.conf`, or a
container's `--ulimit memlock=-1`), not something `applyRealtimePolicy()`
attempts on your behalf.

**Hard real-time** (a guaranteed deadline, not just a statistically tight
one) is not achieved by this alone and requires, on top of the above: a
PREEMPT_RT-patched Linux kernel, and CPU isolation (`isolcpus`, `nohz_full`)
so the scan thread's core isn't shared with other work. **Windows has no
equivalent hard-real-time story** without specialized RTOS extensions
(explicitly out of scope for this project). Actual jitter under load,
`SCHED_FIFO` preemption behavior, and whether memory locking eliminates
page-fault spikes should be measured on the real target with `cyclictest`
(from the `rt-tests` package) against a PREEMPT_RT kernel — this is a field
verification step, not something exercised in this repository's CI/test
suite (which runs unprivileged and on a non-RT kernel).

**Known limitation, deliberately not fixed yet**: `TagStore`'s
`shared_mutex` is a potential priority-inversion / unbounded-wait source
once an external consumer (a future OPC-UA/Modbus server) holds the shared
(read) lock while the scan thread's `write()` needs the exclusive lock. No
such consumer exists yet, so this hasn't been redesigned — see
`docs/roadmap.md`.

## Testing

Each layer is tested in isolation (`tests/tags`, `tests/core`, `tests/io`,
`tests/st/{lexer,parser,interpreter}_test.cpp`) using GoogleTest, fetched via
CMake `FetchContent`. `ScanEngine` is tested primarily through `runOnce()`
for determinism; a few tests exercise the real threaded `start()`/`stop()`
loop, including that falling behind schedule increments `resyncCount`.
`tests/core/rt_scheduling_test.cpp` verifies `applyRealtimePolicy()`
tolerantly (never throws; if a privileged operation didn't take effect, a
warning must explain why) so the suite passes identically whether run
unprivileged (the common case) or as root.

Modbus tests (`tests/net/`, `tests/io/modbus_*_test.cpp`) run against
`tests/support/mock_modbus_server.hpp`/`.cpp`, itself built on nanoMODBUS's
*server* mode rather than a hand-rolled byte-level mock — tests exercise the
same real, spec-correct protocol implementation the production client uses,
plus fault injection (response delay, dropped responses, mid-session
disconnect, connection rejection) at the transport layer. One honest gap:
true connect-*timeout*-under-packet-loss isn't reliably reproducible in a
hermetic sandbox, so those tests cover connection-refused and the timeout
plumbing itself rather than literally induced packet loss.

`SOFTPLC_ENABLE_SANITIZERS` (CMake option) turns on AddressSanitizer/
UndefinedBehaviorSanitizer for dev/CI builds.
