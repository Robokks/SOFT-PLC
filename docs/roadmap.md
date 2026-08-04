# Roadmap

## Phase 1 (this repository, current state)

Core scan-cycle engine, thread-safe tag database, simulated I/O, a real
minimal Structured Text pipeline — now including `DATA_BLOCK`s and
user-defined `FUNCTION_BLOCK`/`FUNCTION` POUs with instance/call-site state
isolation (`st/pou_binder.hpp`; see "Data Blocks, Function Blocks, and
Functions" in `docs/architecture.md`) — opt-in real-time OS scheduling
(priority/affinity/memory locking via `core/rt_scheduling`), and a Modbus
TCP client (master) I/O driver (`io/modbus_tcp_driver.hpp`, built on the
vendored `nanoMODBUS` codec). See `docs/architecture.md`.

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
- Standard `TON`/`TOF`/`CTU`/`CTD` timer/counter FBs (blocked on exposing
  scan-cycle timing as a readable tag first — see Phase 2 below)

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
- **More real I/O drivers**: Modbus RTU (serial), GPIO (e.g. Raspberry Pi),
  NI DAQmx, SPI/I2C — all implementing `IIoDriver` like `ModbusTcpIoDriver`,
  requiring no `ScanEngine` changes. `net::TcpSocket`/`TcpListener` are
  already generic (not Modbus-specific) for reuse by future TCP-based
  fieldbus work.
- **Drive communication**: Profinet, EtherCAT — a larger undertaking than
  Modbus (real-time industrial Ethernet, not plain TCP), likely needing
  dedicated stacks rather than a hand-rolled protocol layer.
- **Modbus TCP driver follow-ups**: gap-tolerant batching (a documented,
  not-yet-implemented optimization — see `docs/architecture.md`), and a
  config-file format for devices/points (today it's a C++
  `ModbusDeviceConfig`/`ModbusPointMapping` API only, not wired into
  `apps/plc_runner`).
- **External tag access**: OPC-UA and/or Modbus server exposing `TagStore`
  to HMI/SCADA clients, exercising the `shared_mutex` concurrent-reader path
  the store was designed for. Once such a consumer exists, revisit
  `TagStore`'s locking: a `shared_mutex` is a priority-inversion / unbounded-
  wait risk once something can hold the shared (read) lock while the scan
  thread's `write()` needs the exclusive lock — deliberately not fixed now
  since there's no real consumer yet to justify the complexity. Likely fix:
  per-tag atomics or a seqlock/double-buffer scheme for scalar types, to
  bound the scan thread's worst-case wait time.
- **Standard timer/counter FBs**: `TON`/`TOF`/`CTU`/`CTD`, ship as ordinary
  ST-defined `FUNCTION_BLOCK`s using the Phase 1 clone-and-rebind mechanism
  (same as the `R_TRIG`-style edge detector already tested), once
  `ScanContext`'s cycle timing is exposed as a readable tag.
- **Expanded ST language surface**: `FOR`, `CASE`, function calls in
  expression position, arrays/structs, remaining elementary types,
  `VAR_IN_OUT` parameters, `VAR_OUTPUT` write-protection, real `%DB`
  numbered/byte-offset addressing, multi-file compilation units.
- **Persistence**: retentive (`VAR RETAIN`) variables surviving a restart.
- **HMI/Web UI**: a way to observe/force tag values without recompiling a
  program (out of scope until the above runtime pieces are solid).
