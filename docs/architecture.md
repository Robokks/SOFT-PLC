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
  server/ plc_server.hpp — HTTP programming/monitoring API (see below)
src/                        (matching .cpp files)
apps/plc_runner/            main.cpp — loads an .st file and runs the engine
apps/plc_server/            main.cpp — hosts PlcServer's HTTP API, program optional
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

## Data Blocks, Function Blocks, and Functions (`st/pou_binder`)

A single `.st` source can now define `DATA_BLOCK`s and `FUNCTION_BLOCK`/
`FUNCTION` POUs (Program Organization Units) alongside exactly one
`PROGRAM`, in any order. `Parser::parseCompilationUnit()` collects them into
a `CompilationUnit{pous, dataBlocks, program}`; `bindCompilationUnit()`
(`st/pou_binder.{hpp,cpp}`) turns that into the same flat, fully-`TagId`-
resolved `StProgramAst` the interpreter always ran, so `Interpreter` itself
gained only one new statement kind (`CallStmt`) and no new concept of scope.

**Why clone-and-rebind instead of dynamic scope resolution.** The scan-cycle
hot path's core invariant — every `TagId` resolved once at load time, never
by name during a scan (see "Tag database" above) — had to survive FB
instantiation. Rather than give the interpreter a runtime notion of "current
instance" and resolve names against it on every access, each FB instance
declaration and each FC call site gets its own deep-cloned copy of the
callee POU's body (`Expr`/`Stmt::clone()`), bound against that instance's
own freshly-declared tags, exactly like the top-level `PROGRAM` always was.
The cost is one clone-and-bind pass per instance/call-site, paid once at
load time and proportional to program size — not to scan count — which is
an easy trade for a system that scans at kHz and loads once. The bound
result is stored as a `Frame` (cloned body + `VAR_TEMP` reset list) in
`StProgramAst::frames`; a `CallStmt` only ever holds a plain `frameIndex`
into that vector, mirroring how `IdentifierExpr` only ever holds a `TagId`.

**Clone granularity** differs deliberately by POU kind: an FB instance is
cloned **once per instance declaration** (`Motor1 : FB_MotorControl;`) and
that one `Frame` is reused by every `CallStmt` invoking `Motor1`, since
re-executing the same bound tree against the same persistent tags is safe
and correct — that persistence *is* the point of an instance. An FC call is
cloned **once per call site**, never shared even between two calls to the
same FC type, so each call site's `VAR_OUTPUT`/`VAR_TEMP` storage stays
individually addressable and inspectable (e.g. for a future online-monitor
view) rather than aliasing another call site's scratch space.

**Naming and scoping.**
- `DATA_BLOCK DB1 VAR Speed : INT; END_VAR END_DATA_BLOCK` declares
  `"DB1.Speed"` directly into `TagStore` — a data block is mechanically "a
  persistent named record with no code," so it reuses `TagStore`'s existing
  flat name-keying with zero new addressing machinery. Real Siemens-style
  numbered/byte-offset `%DB1.DBX0.0` addressing is a deferred, unrelated
  feature (see below); `DB1` here is purely a naming convention, not a
  memory area.
- An FB instance's members are declared under `"<InstanceName>.<Member>"`
  (`Motor1.Running`, and recursively `Outer1.Inner.Out1` for nested
  instances), reusing the same mechanism.
- **Inside** a POU body, members — including a nested instance's members —
  are referenced by bare name (`Speed`, `TmpTimer.Q`): the cloned body has
  no idea which instance prefix it was bound under, which is exactly what
  makes the clone-and-bind step a straightforward recursive reuse of the
  top-level `PROGRAM` binder. `DATA_BLOCK` members are the one exception —
  referenced by full dotted name from anywhere, since they're global storage
  rather than scoped to a call.
- Nothing currently stops code outside an instance from writing its
  `VAR_OUTPUT` members directly (`Motor1.Running := TRUE;` compiles) —
  flagged as a known gap below, not fixed, since it needs a write-visibility
  concept `TagStore` doesn't have yet.

**Call syntax** is statement-only in v1 (no function calls in expression
position): `CalleeName(Param := inputExpr, Param => outputTarget);`. This
"box with named in/out pins" shape is deliberately the same shape a future
Ladder Diagram front-end needs for an FB instance placed inline on a rung,
so `CallStmt`/`Frame` are built to be reusable there rather than needing a
second calling convention later. An FB call may omit any `VAR_INPUT` (an
undriven pin keeps its last value, matching Siemens semantics for a
persistent instance); an FC call must bind every `VAR_INPUT` explicitly —
omitting one is a load-time error, since FC call-site storage is a hidden
implementation detail rather than a real instance a user would think to
re-drive between calls.

**Type coercion on write.** A bare integer literal (`200`) is always parsed
as `DINT` (`Parser::parsePrimary()` has no knowledge of the assignment
target's declared type at parse time). `Interpreter` therefore narrows every
value written through an `AssignStmt` target or a `CallArg` binding to that
target's actual declared `TypeId` (`coerceToType()` in `interpreter.cpp`,
alongside the existing `fromDouble`/`asDouble` numeric-promotion helpers
arithmetic already used) rather than assuming the evaluated `Value`'s active
variant alternative already matches — writing e.g. a `DINT`-valued literal
into an `INT`-declared tag narrows to `int16_t` instead of leaving a
type/variant mismatch latent until the next read. Assigning between
genuinely incompatible types (e.g. `BOOL` into `DINT`) still throws
`std::runtime_error` at run time.

**Cycle detection.** Recursive POU call graphs — an FB instantiating itself,
directly or through another FB, or (in principle) an FC calling itself —
have no well-defined output for clone-and-rebind (unbounded recursive
cloning) and are rejected with `std::runtime_error` at load time via a
"currently expanding" name stack in the binder, not left to overflow the
stack or loop forever.

**v1 scope, and what's explicitly deferred** (not silently dropped — tracked
in `docs/roadmap.md`): standard `TON`/`TOF`/`CTU`/`CTD` timer/counter FBs
(blocked on exposing scan-cycle timing as a readable tag first); function
calls in expression position; real `%DB` numbered/byte-offset addressing;
arrays/structs; multi-file compilation units (today: one `.st` source with
all POU/DB/PROGRAM definitions); `VAR_OUTPUT` write-protection from outside
an instance; `VAR_IN_OUT` pass-by-reference parameters.

## Ladder Diagram (`RUNG` statements in the ST pipeline)

There is no graphical programming surface yet (see `docs/roadmap.md`), so v1
of Ladder Diagram support is a textual statement, `RUNG`, added directly to
the same lexer/parser/binder/interpreter Structured Text already uses —
deliberately *not* a separate front-end/compiler module, since a rung's
semantics turn out to already be expressible with the grammar that existed:

```
RungStmt   ::= 'RUNG' Expression '=>' RungOutput (',' RungOutput)* ';'
RungOutput ::= ('SET' | 'RESET')? DottedIdentifier
```

**Contacts and branches are just the existing boolean expression grammar.**
A normally-open contact is a `BOOL` tag reference; a normally-closed contact
is `NOT tag` (already implemented); series contacts are `AND`; a parallel
branch is `OR` — precisely ladder's continuity semantics, so `RungStmt`
reuses `parseExpression()`/`Expr`/`evaluate()` as-is rather than inventing
dedicated `Contact`/`SeriesGroup`/`ParallelGroup` AST nodes. A classic
seal-in circuit is therefore just:
```
RUNG (Start OR Motor) AND NOT Stop => Motor;
```
— `Motor` read back on the right-hand side of its own rung is the standard
ladder "feedback contact" idiom, and needs no special-casing: it's an
ordinary `IdentifierExpr` resolving to the same `TagId` the coil writes.

**Coils** (`RungOutput`) come in three kinds, matching standard ladder coil
types: `Direct` (no modifier) writes the rung's boolean result to the target
every scan, exactly like a continuously-assigned output; `SET` forces the
target `TRUE` only while the rung is powered and otherwise leaves it
untouched (a latch); `RESET` symmetrically forces `FALSE` while powered.
Multiple coils can share one rung (`RUNG cond => SET A, RESET B;`), each
with an independently-chosen kind. A coil target must resolve to a `BOOL`
tag — `pou_binder` throws `std::runtime_error` at load time otherwise
(coils can address a plain output tag, a DB member, or an FB instance
member, same as any other dotted name).

**FB/FC boxes "on" a rung.** True graphical ladder places a function block
box inline on a rung, with its own `EN`/`ENO` power-flow pins. Without a GUI
to place boxes on a 2-D grid, v1 achieves the same effect textually: an FB
call is an ordinary `CallStmt` (see "Data Blocks, Function Blocks, and
Functions" above) placed immediately before the `RUNG` that reads its
output or drives its input, e.g.:
```
Edge1(CLK := StartButton);
RUNG Edge1.Q AND NOT StopButton => Motor;
```
`Edge1.Q` is read exactly like any other contact once the call has run. This
is a deliberate, documented simplification rather than an oversight — a
future GUI can still emit this exact statement sequence (or construct the
same `CallStmt`/`RungStmt` AST nodes directly, bypassing the text grammar
entirely), so nothing here needs to change once box-on-rung placement with
real `EN`/`ENO` power-flow semantics is designed.

**Known limitation, deliberately not fixed yet**: no `EN`/`ENO` power-flow
propagation through an FB box (a called FB always executes regardless of
rung state — v1's `CallStmt` has no "enable" input); no visual/graphical
representation at all (this is a textual stand-in, not a renderer); a
`RUNG`'s condition is evaluated and every coil driven exactly once per
scan, so a rung cannot itself branch into multiple independently-powered
sub-rungs the way a 2-D ladder grid with multiple output columns can
(model that today as multiple separate `RUNG` statements instead).

## Standard library: TON/TOF/CTU/CTD (`st/standard_fbs.hpp`)

The four standard IEC 61131-3 timer/counter function blocks are implemented as
ordinary ST-source `FUNCTION_BLOCK`s — the same clone-and-rebind mechanism as
any user-defined FB — rather than as native C++ intrinsics wired into the
interpreter. `include/softplc/st/standard_fbs.hpp` holds their source text
(`kStandardFbLibrarySource`); `StProgram::load()` parses it with a new
`Parser::parsePouLibrary()` (loops `FUNCTION_BLOCK`/`FUNCTION` definitions
only — no `DATA_BLOCK`, no `PROGRAM` required, unlike `parseCompilationUnit()`)
and appends the result to the user's own `CompilationUnit::pous` before
binding. An unused standard FB costs nothing beyond the one-time parse of a
small fixed text; only instantiating one pays the usual per-instance
clone/bind cost, same as any other FB.

**Why not native intrinsics.** Every other piece of stateful ST behavior in
this codebase (the `R_TRIG`-style edge detector used as Phase A's own proof
point) already goes through clone-and-rebind; giving timers/counters a
special native path would mean two different execution models for
"persistent FB instance state" with no real benefit — the ST language is
already expressive enough to define them correctly.

**Exposing scan time**: `TON`/`TOF` need to know how much wall-clock time
elapsed each scan. `ScanEngine` now resolves a well-known global TIME tag,
`"System.CycleTime"`, once at construction (`tags_.find(...)`, cached as an
`optional<TagId>` — absent, and skipped every scan at zero cost, for any
`TagStore` that never went through `st::bindCompilationUnit()`, e.g. a plain
`NativeProgram`-based engine). Every `runOnce()` writes the *actual* elapsed
time since the previous scan started into that tag, before executing the
program — not the configured (target) cycle time, so timers stay accurate
under jitter or overrun. The very first scan has no previous sample, so it
publishes the configured cycle time as a reasonable default.
`bindCompilationUnit()` unconditionally declares `"System.CycleTime"` for
every compiled program, so any POU body can read it exactly like a
`DATA_BLOCK` member (`resolveName()`'s existing TagStore-fallback path) with
no special syntax. `TON`/`TOF` accumulate `ET` by adding this per-scan
sample each execution while running, clamping at `PT` — a discrete,
scan-quantized integration (accuracy bounded by the scan rate), not a
free-running wall-clock read, deliberately: it keeps timer state advancing
in lockstep with the same deterministic scan model every other tag in this
runtime uses.

**CTU/CTD parameter names.** The real IEC 61131-3 standard names these
blocks' reset/load inputs `R`/`LD` (not `RESET`) — used here verbatim, which
incidentally sidesteps any collision with the `RUNG` statement's `SET`/
`RESET` coil keywords (see "Ladder Diagram" above); `TON`/`TOF`/`CTU`/`CTD`
themselves use `IN`/`PT`/`Q`/`ET`/`CU`/`CD`/`PV`/`CV`, none of which are
reserved words in this grammar.

**Reserved names**: a user `CompilationUnit` that defines its own
`FUNCTION_BLOCK`/`FUNCTION` named `TON`/`TOF`/`CTU`/`CTD` fails to load —
merging the standard library after parsing the user's source hits the same
duplicate-POU-name check `bindCompilationUnit()` already uses for any other
name collision.

**Known limitation, deliberately not fixed yet**: `TOF`'s `ET` does not
reset to zero once `Q` goes false (it holds at `PT` until `IN` rises again),
which matches this v1's chosen semantics but not every IEC-compliant
implementation's exact edge-case behavior — documented here rather than
silently diverging.

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

## Modbus RTU (serial) I/O driver (`io/modbus_rtu_*`, `net/serial_port`)

`ModbusRtuIoDriver` is the serial counterpart to `ModbusTcpIoDriver`: same
`IIoDriver` contract, same freeze-last-good-value/never-blocks-the-scan-
thread semantics, same batching via the transport-agnostic
`buildBatches()`/`decodeModbusPointValue()`/`encodeModbusPointValue()` (now
shared by both drivers — see below). It reuses `nanoMODBUS`'s RTU transport
mode rather than hand-rolling serial framing/CRC, exactly as the TCP driver
reuses its TCP/MBAP mode.

**Why one poller thread per bus, not per device.** `ModbusTcpIoDriver` gives
each configured device its own socket and its own poller thread, since a TCP
connection is inherently point-to-point. A serial line is different: RS-485
(and even a point-to-point RS-232 link relayed through a single USB adapter)
is a **shared, half-duplex medium** — every device on it sees every byte,
and only one conversation can be in flight at a time. Two threads issuing
requests on the same serial port concurrently would corrupt the bus. So the
new config shape reflects that directly: `ModbusRtuBusConfig` is one
physical port (`devicePath`, baud/parity/stop-bits `net::SerialConfig`) that
owns a list of `ModbusRtuDeviceConfig`s (each just a `unitId` + its own
points + its own health tag), and `ModbusRtuIoDriver` starts exactly **one**
thread per bus, which polls every device on it sequentially each cycle
(`pollInterval` applies to one full pass over the bus, not per device).

**`ModbusRtuBus`** is the RTU analog of `ModbusClient`: one `net::SerialPort`
+ one nanoMODBUS client instance per bus, shared across every unit id
configured on it. Each of its `readBatch`/`writeRegisters`/`writeCoils`
methods takes the target `unitId` explicitly and calls
`nmbs_set_destination_rtu_address()` before issuing the request — the
mechanism that actually addresses one slave out of several sharing the same
wire. `ModbusRtuIoDriver`'s single poll loop calls these once per device,
per cycle, always from the same thread.

**A real bug this surfaced and fixed**: `nmbs_set_destination_rtu_address()`
turned out to control the outgoing unit-id byte for *both* transports —
nanoMODBUS applies `nmbs->msg.unit_id = nmbs->dest_address_rtu` unconditionally
in `nmbs_send()`, not just under `NMBS_TRANSPORT_RTU` (its name is a slight
misnomer). The original TCP `ModbusClient::ensureConnected()` never called
it, so a configured `ModbusDeviceConfig::unitId` was silently never sent —
harmless against a plain Modbus TCP device (which ignores the byte), but
wrong for a Modbus TCP-to-RTU gateway addressing a specific downstream RTU
slave. Fixed by adding the missing call once the TCP client is created,
alongside the read/byte timeouts.

**`net::SerialPort`** (`net/serial_port.hpp`) mirrors `net::TcpSocket`
deliberately closely: same `recvSome()`/`sendSome()` byte-count/timeout
contract (so it plugs into nanoMODBUS's transport callbacks exactly like
`TcpSocket` does), same movable-not-copyable/`adopt()`-for-tests shape.
POSIX `open()` configures the port via `termios` (`cfmakeraw` for an 8-bit-
clean line with no canonical-mode/echo/signal processing, explicit baud via
`cfsetispeed`/`cfsetospeed` from a small fixed table of standard rates,
data bits/parity/stop bits from `SerialConfig`) and uses `select()` +
non-blocking I/O for per-call timeouts, since there's no serial equivalent
of `SO_RCVTIMEO`. Windows uses `CreateFileA`/`DCB`/`COMMTIMEOUTS` — written
to the same documented Win32 serial APIs `net::TcpSocket`'s Winsock path
uses, but (like the rest of this project's Windows code) not exercised by
this repository's Linux-only test suite.

**Shared decode/encode/diagnostics, not duplicated.** Word-order-aware
value encode/decode (`decodeModbusPointValue()`/`encodeModbusPointValue()`)
and `ModbusDeviceDiagnostics` used to live inside `modbus_tcp_driver.cpp`/
`.hpp`; both moved to `modbus_mapping.{hpp,cpp}` so the RTU driver reuses
them verbatim instead of drifting out of sync with a second copy. The
per-transport protocol-request logic (the `nmbs_read_*`/`nmbs_write_*`
switch bodies in `ModbusClient`/`ModbusRtuBus`) was judged different enough
in shape (extra `unitId` parameter, different connect/open step) to leave
duplicated rather than force through a transport-abstraction interface —
each file stays independently readable, and nanoMODBUS error translation
(the one piece that really would drift if duplicated) is factored into a
small shared internal header, `src/io/modbus_nmbs_error.hpp`.

**Testing** uses a real pseudo-terminal (PTY) pair rather than a hand-rolled
byte mock, the serial equivalent of the TCP driver's approach: `SerialPort`
tests drive the PTY master fd directly against a `SerialPort` opened on the
slave path; `tests/support/mock_modbus_rtu_server.{hpp,cpp}` runs
nanoMODBUS's own server mode over the same PTY, configured with a fixed
`address_rtu` — a request addressed to any other unit id on that server
gets no response at all (real multi-drop-bus behavior), which is the
concrete test proof that `ModbusRtuBus` really transmits the `unitId` it's
given rather than a fixed/ignored value.

**Known limitations, deliberately not fixed yet**: no config-file format
(same gap as TCP — C++ struct construction only); a bus's single poller
thread means a hung/slow device on that bus delays polling every other
device sharing its port (inherent to a shared half-duplex medium, not
fixable without violating the "never two threads on one serial port"
constraint — configure one device per bus if independent polling cadence
matters more than sharing a physical port); gap-tolerant batching (same
documented, not-yet-implemented optimization as TCP).

## Programming/monitoring HTTP server (`server/`, `apps/plc_server`)

`PlcServer` is the first piece of "external tag access" from `docs/roadmap.md` and the
backend half of the planned browser-based GUI: an HTTP API in front of a live PLC
runtime that can compile/"download" a new ST source into a running target and expose
the live `TagStore` for an online tag monitor. `apps/plc_server` is a thin executable
mirroring `plc_runner`'s shape, except an initial program is optional -- the server can
sit idle with nothing loaded until a programming client connects, like a real PLC
target waiting for a download.

**Whole-object-graph swap on download, not an in-place patch.** A new program can
declare an entirely different tag set from the one currently running, so `TagId`s from
the old `TagStore` have no meaning against a patched one. `PlcServer::download()`
therefore compiles the incoming source against a **fresh** `TagStore` first (so a bad
compile never disturbs the running target -- `st::StProgram::load()`'s exception leaves
the old `{TagStore, StProgram, ScanEngine}` untouched), then, only on success, destroys
the old `ScanEngine` (joining its scan thread) before constructing and starting a new
one over the new `TagStore`/`StProgram`. The old engine is always fully stopped before
the new one starts, rather than briefly overlapped, because both engines would
otherwise call into the same long-lived `IIoDriver` concurrently -- no `IIoDriver`
implementation here is designed or tested for two `ScanEngine`s driving it at once.
This means a download causes a brief stop (bounded by one scan's `stop()`/`start()`),
matching how a real PLC's download briefly goes to STOP; a true hot-patch that
preserves `TagId`s/state across a download is future work once there's a concrete need
for it. The `IIoDriver` itself is long-lived across downloads: `readInputs()`/
`writeOutputs()` take the `TagStore` as a parameter rather than caching it (see
"I/O abstraction" above), so handing them a fresh `TagStore` each download is
transparent -- Modbus device configuration, for instance, survives a download
unchanged.

**Locking**: `PlcServer` guards its `{TagStore, StProgram, ScanEngine}` unit with its
own `shared_mutex` (`stateMutex_`), separate from `TagStore`'s own internal one --
HTTP handlers (running on cpp-httplib's worker threads) take the shared lock to read
tags/diagnostics/status, `download()` takes the exclusive lock to swap the whole unit.

**Endpoints** (v1, all plain HTTP, no auth/TLS -- same trusted-local-network threat
model as this project's existing Modbus TCP/RTU drivers): `GET /api/status` (running
state, program name, tag count, `ScanDiagnostics`), `GET /api/tags` (a full JSON tag
snapshot via the new `TagStore::snapshot()`), `POST /api/program` (body = raw ST
source; 200 + program name/tag count on success, 400 + `{"error":...}` on failure,
leaving any prior program running), `POST /api/tags/<name>` (body =
`{"value": <bool|number|string>}`; a tag write/"force" -- see below), and
`GET /api/tags/stream` -- a Server-Sent-Events tag snapshot every 200ms until the
client disconnects.

**Tag write/"force" reuses `coerceToType()`, doesn't reimplement it.** `AssignStmt`'s
narrowing rule (a bare integer literal is always `DINT`; every write coerces to the
target tag's *declared* type -- see "Type coercion on write" above) is exactly what a
JSON-typed write needs too: a JSON number decoded as a plain `double` must narrow into
whatever numeric type the target tag declares, the same way a `DINT` literal narrows
into an `INT` tag. Rather than re-deriving that logic HTTP-side, `coerceToType()` (and
its numeric helpers `isNumeric`/`asDouble`/`fromDouble`) moved from being
`st::interpreter.cpp`-local to public functions in `tags/value.{hpp,cpp}` -- they were
already pure `Value`/`TypeId` operations with nothing ST-specific about them, so this
is a relocation, not a rewrite; `Interpreter` calls the same functions unqualified via
`using` declarations. `PlcServer::writeTag()` decodes the request body with a
deliberately narrow hand-written parser (`parseValueField()` in `plc_server.cpp`,
extracting just the one `"value"` field's literal -- see "JSON is hand-written" below)
into a `bool`/`double`/`std::string` `Value`, then calls `coerceToType()` exactly like
`AssignStmt` does; `TIME` is the one case special-cased outside `coerceToType()`
(a JSON number decoding into a `TimeValue` millisecond count), since no other caller
ever needed a bare-double-to-`TIME` conversion.

**Why SSE, not WebSocket, for the live monitor.** The monitor only needs one direction
(server -> browser); a future "force tag" write is an ordinary `POST`, not something
that needs a persistent duplex channel. SSE is plain chunked HTTP text
(`text/event-stream`), so it needed no protocol library beyond the HTTP layer already
in place -- adding a WebSocket dependency (or hand-rolling RFC 6455 framing) for
one-directional data had no payoff here. Known limitation, deliberately not fixed yet:
the stream polls a full snapshot on a fixed 200ms wall-clock interval rather than being
scan-synchronized or diff-based (fine for a human-facing monitor UI, not a
control-loop feed), and holds one httplib worker thread per connected client for the
connection's lifetime (fine for a handful of GUI viewers, not designed for many
concurrent watchers).

**Why cpp-httplib.** MIT-licensed, single-header, `FetchContent`-fetchable like
nanoMODBUS/googletest (`cmake/FetchCppHttplib.cmake`, pinned to `v0.15.3`), and its
optional OpenSSL/zlib/brotli backends are explicitly forced off in that same CMake
file so this stays a plain-HTTP build with no implicit extra runtime dependency picked
up just because a build machine happens to have one of those installed.

**JSON is hand-written, not a vendored library.** `Value`'s type set is eight simple
variants (`tags::toJson()` in `value.cpp`), and the only *incoming* JSON this v1 needs
to parse is one fixed shape, `{"value": <literal>}` for the tag-write endpoint
(`parseValueField()` in `plc_server.cpp` -- a few lines that find the `"value"` key and
decode whichever JSON literal follows it, not a general recursive-descent JSON
parser). Between that and the hand-written emit side (plus `jsonEscapeString()`,
shared between `Value` string values and tag names), a general JSON library still
isn't earning its keep. Revisit this once an endpoint needs to parse genuinely
structured/nested JSON input (e.g. a future graphical-ladder-editor payload, which
is expected to need real nesting a one-field extractor can't handle).

**Testing** (`tests/server/plc_server_test.cpp`) follows this project's established
real-server-not-a-mock approach (mirroring the Modbus mock-server tests): a real
`PlcServer` bound to an OS-assigned ephemeral port (`bindEphemeralPort()`/
`listenAfterBind()`, added alongside the normal fixed-port `listen()` specifically for
this), driven through a real `httplib::Client` -- covering a fresh server with nothing
loaded, a successful download starting the engine, an invalid download leaving the
prior program running untouched, `/api/tags` reflecting live scan-driven updates, and
at least one real SSE frame received over `/api/tags/stream`.

**Known limitations, deliberately not fixed yet**: no authentication/TLS (v1 assumes a
trusted local network, like Modbus); no static-file serving for a frontend bundle yet
(deferred until a GUI frontend actually exists to serve); download always takes the
raw-ST-source path -- a future graphical ladder editor is expected to either emit the
same textual grammar or a JSON IR compiled server-side directly into
`CallStmt`/`RungStmt` AST nodes (see "Ladder Diagram" above), neither of which exists
yet; the tag-write endpoint takes one value at a time (no batch/multi-tag write).

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
`tests/st/{lexer,parser,pou_binder,interpreter}_test.cpp`) using GoogleTest,
fetched via CMake `FetchContent`. `tests/st/pou_binder_test.cpp` is the
concrete proof of the clone-and-rebind design: two instances of the same FB
type get independently-addressable, independently-stateful `TagId`s; nested
instance-of-instance produces four independently-addressable groups; two FC
call sites don't leak `VAR_TEMP` into each other; and each of the binder's
load-time error cases (unresolved instance type, unknown call-arg name,
wrong-direction `:=`/`=>` binding, omitted required FC input, circular FB
instantiation) is exercised directly. `RUNG` statements are covered across
the same three layers: parser tests for direct/SET/RESET coils and
multi-coil rungs; a binder test for the non-BOOL-coil-target error; and
interpreter tests for a seal-in latch via feedback contact, SET/RESET
latching without feedback, and an FB instance's output read as a contact on
an adjacent rung. `tests/st/standard_fbs_test.cpp` covers TON/TOF/CTU/CTD via
a `tick()` helper that drives one scan deterministically (writes an explicit
`System.CycleTime` sample, then calls `execute()` directly) rather than
relying on real `sleep_for`-based timing, plus the reserved-name collision
error; `tests/core/scan_engine_test.cpp` separately verifies `ScanEngine`
itself publishes the configured cycle time on the first scan and a real
measured elapsed duration on subsequent scans. `ScanEngine` is tested primarily through `runOnce()`
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

`tests/server/plc_server_test.cpp` covers the HTTP programming/monitoring API (see
"Programming/monitoring HTTP server" above) the same real-server-not-a-mock way.

Modbus RTU tests (`tests/net/serial_port_test.cpp`, `tests/io/modbus_rtu_*`)
follow the same real-protocol-not-a-mock philosophy over a real PTY pair
(see "Modbus RTU (serial) I/O driver" above): `SerialPort` byte transfer and
timeout behavior, `ModbusRtuBus` round-trips for every register type plus
the mismatched-unit-id-times-out and genuine-Modbus-exception cases
(mirroring `ModbusClient`'s TCP test suite), and `ModbusRtuIoDriver`
end-to-end through a `TagStore` (poll-and-observe, drop-in `IIoDriver` via
`ScanEngine`, device-offline freeze-last-good-value + health tag) — the same
four scenarios `ModbusTcpIoDriver`'s tests cover, adapted to the bus/unit-id
shape.

`SOFTPLC_ENABLE_SANITIZERS` (CMake option) turns on AddressSanitizer/
UndefinedBehaviorSanitizer for dev/CI builds.
