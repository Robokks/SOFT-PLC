# Roadmap

## Phase 1 (this repository, current state)

Core scan-cycle engine, thread-safe tag database, simulated I/O, a real
minimal Structured Text pipeline — now including `DATA_BLOCK`s and
user-defined `FUNCTION_BLOCK`/`FUNCTION` POUs with instance/call-site state
isolation (`st/pou_binder.hpp`; see "Data Blocks, Function Blocks, and
Functions" in `docs/architecture.md`), a textual Ladder Diagram `RUNG`
statement reusing the same grammar/interpreter (see "Ladder Diagram"), and
the standard `TON`/`TOF`/`CTU`/`CTD` timer/counter FBs shipped as ST source
(see "Standard library: TON/TOF/CTU/CTD") — opt-in real-time OS scheduling
(priority/affinity/memory locking via `core/rt_scheduling`), and Modbus TCP
and RTU (serial) client (master) I/O drivers (`io/modbus_tcp_driver.hpp`,
`io/modbus_rtu_driver.hpp`, both built on the vendored `nanoMODBUS` codec —
see "Modbus RTU (serial) I/O driver" in `docs/architecture.md` for how the
two share their point-mapping/encode-decode/diagnostics layer while
differing in threading model, since a serial bus is shared/half-duplex
where a TCP connection is point-to-point). See `docs/architecture.md`.

Explicitly out of scope for Phase 1's ST subset (parser will reject these):
- `FOR` loops, `CASE` statements, `REPEAT` loops
- Arrays and structured (`STRUCT`) types
- Additional elementary types: `SINT/USINT/UINT/UDINT/LINT/ULINT/WORD/DWORD/LWORD`
- Multiple POUs scheduled at different rates/priorities within one engine
  (`ScanEngine` currently hosts exactly one top-level `IProgram`)
- Function calls in expression position (`X := FC_Foo(In1 := 1);` — v1 only
  supports calls as their own statement)
- Real Siemens-style numbered/byte-offset `%DB1.DBX0.0` addressing (today's
  `DATA_BLOCK` members are addressed by dotted name only, e.g. `DB1.Speed`)
- Multi-file compilation units (one `.st` source holds all `DATA_BLOCK`/
  `FUNCTION_BLOCK`/`FUNCTION`/`PROGRAM` definitions)
- `VAR_IN_OUT` pass-by-reference parameters
- `VAR_OUTPUT` write-protection from outside the owning instance (nothing
  currently stops `Motor1.Running := TRUE;` from outside `Motor1`)

## Phase 2 and beyond

- **Ladder Logic (LD)**: a first textual cut has landed — the `RUNG`
  statement (see "Ladder Diagram" in `docs/architecture.md`), reusing the
  existing boolean expression grammar for series/parallel contacts and
  adding Direct/SET/RESET coils, with FB/FC boxes placed as an adjacent
  `CallStmt`. Still needed: a real graphical editor/renderer (blocked on the
  GUI phase — see below), `EN`/`ENO` power-flow propagation into FB calls,
  and multi-output-column rungs (today: one `RUNG` per independently-powered
  output group).
- **Function Block Diagram (FBD)**: not started; implement as an additional
  front-end compiling down to the existing `Expr`/`Stmt` AST, reusing the
  scan engine, tag database, and I/O abstraction unchanged, same as LD.
- **Instruction List (IL)**: same approach; note IL is deprecated in newer
  IEC 61131-3 editions, so prioritize it below LD/FBD.
- **Task / multi-POU scheduling**: an IEC "Task" concept binding one or more
  programs to independent scan rates/priorities, replacing `ScanEngine`'s
  current single-`IProgram` model.
- **More real I/O drivers**: Modbus RTU shipped (`io/modbus_rtu_driver.hpp`,
  `net/serial_port.hpp`); GPIO (e.g. Raspberry Pi), NI DAQmx, SPI/I2C still
  open — all implementing `IIoDriver` like the existing Modbus drivers,
  requiring no `ScanEngine` changes. `net::TcpSocket`/`TcpListener` are
  already generic (not Modbus-specific) for reuse by future TCP-based
  fieldbus work; `net::SerialPort` likewise for future serial-based work
  (e.g. a non-Modbus SCADA protocol).
- **Drive communication**: Profinet, EtherCAT — a larger undertaking than
  Modbus (real-time industrial Ethernet, not plain TCP), likely needing
  dedicated stacks rather than a hand-rolled protocol layer.
- **Modbus driver follow-ups** (TCP and RTU): gap-tolerant batching (a
  documented, not-yet-implemented optimization — see `docs/architecture.md`),
  and a config-file format for devices/points/buses (today it's a C++
  `ModbusDeviceConfig`/`ModbusRtuBusConfig`/`ModbusPointMapping` API only,
  not wired into `apps/plc_runner`). RTU-specific: the Windows
  `net::SerialPort` path (`CreateFileA`/`DCB`/`COMMTIMEOUTS`) is written but
  not exercised by this repository's Linux-only test suite.
- **External tag access**: an HTTP programming/monitoring server
  (`server/plc_server.hpp`, `apps/plc_server` — see "Programming/monitoring
  HTTP server" in `docs/architecture.md`) now exposes `TagStore` (a JSON
  snapshot, an SSE live-tag stream, and a `POST /api/tags/<name>` write/
  "force" endpoint) and can compile/"download" a new program into a running
  target — the first real concurrent external consumer exercising the
  `shared_mutex` concurrent-reader path the store was designed for, though
  not yet at a load that makes the locking-revisit below urgent. Still open:
  OPC-UA/Modbus-server access for HMI/SCADA clients specifically (a separate,
  not-yet-built consumer of the same `TagStore`). Once concurrent-reader load
  actually matters, revisit `TagStore`'s locking: a `shared_mutex` is a
  priority-inversion / unbounded-wait risk once something can hold the shared
  (read) lock while the scan thread's `write()` needs the exclusive lock —
  deliberately not fixed now since real measured contention doesn't exist yet
  to justify the complexity. Likely fix: per-tag atomics or a
  seqlock/double-buffer scheme for scalar types, to bound the scan thread's
  worst-case wait time.
- **Standard timer/counter FBs**: shipped — `TON`/`TOF`/`CTU`/`CTD` are
  ordinary ST-defined `FUNCTION_BLOCK`s (`st/standard_fbs.hpp`) accumulating
  against the new `"System.CycleTime"` global tag `ScanEngine` publishes
  each scan (see `docs/architecture.md`). Still open: `TOF`'s `ET` doesn't
  reset to zero after timeout (documented v1 semantics, not every
  IEC-compliant implementation's exact edge case).
- **Expanded ST language surface**: `FOR`, `CASE`, function calls in
  expression position, arrays/structs, remaining elementary types,
  `VAR_IN_OUT` parameters, `VAR_OUTPUT` write-protection, real `%DB`
  numbered/byte-offset addressing, multi-file compilation units.
- **Persistence**: retentive (`VAR RETAIN`) variables surviving a restart.
- **HMI/Web UI — graphical programming interface**: a browser-based, TIA-Portal-style
  front end (program editor, "download" to a running target, live online monitor).
  Decided direction: a web app talking to the backend HTTP API, a true graphical
  (2-D grid) Ladder Diagram editor rather than a textual stand-in, real Instruction
  List (IL/STL) language support (not just ST), with v1 targeting the full loop
  (editor + download + live monitor). Landed so far: the backend half —
  `server/plc_server.hpp`/`apps/plc_server` (see "Programming/monitoring HTTP server"
  in `docs/architecture.md`) — compile/download, a live tag monitor (JSON snapshot +
  SSE stream), and a tag write/"force" endpoint, all over HTTP. A minimal web
  frontend has also landed (`web/` — Vite + React + TypeScript, see "Web frontend" in
  `docs/architecture.md`): status bar, live tag table with per-tag force, and a raw
  ST-source textarea + Download button, with `PlcServer` able to serve the built
  bundle directly (`--static-dir`) — the full editor+download+live-monitor loop works
  end-to-end today, verified by hand, for plain ST source. Still open, in roughly
  dependency order: a real graphical Ladder editor that serializes to/from
  `RungStmt`/`CallStmt` (either via the existing textual `RUNG` grammar or a JSON IR
  compiled server-side directly into those AST nodes — see "FB/FC boxes on a rung" in
  `docs/architecture.md`) in place of today's plain textarea; a genuine IL/STL front
  end compiling its accumulator+jump model down into the existing `Expr`/`Stmt` tree
  (`ast.hpp` has no label/goto construct today, so this needs either a restricted
  structured-jump subset or a new jump-capable execution primitive — a real design
  decision, not yet made); and wiring `web/`'s `npm run build` into the CMake build
  (the two build systems are independent for now). A real graphical Ladder *editor UI*
  (drag-and-drop rungs, etc.) is inherently a large, multi-session undertaking —
  expect it to land incrementally, each slice with its own tests, rather than as one
  change.
