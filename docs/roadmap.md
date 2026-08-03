# Roadmap

## Phase 1 (this repository, current state)

Core scan-cycle engine, thread-safe tag database, simulated I/O, a real
minimal Structured Text pipeline, opt-in real-time OS scheduling
(priority/affinity/memory locking via `core/rt_scheduling`), and a Modbus
TCP client (master) I/O driver (`io/modbus_tcp_driver.hpp`, built on the
vendored `nanoMODBUS` codec). See `docs/architecture.md`.

Explicitly out of scope for Phase 1's ST subset (parser will reject these):
- `FOR` loops, `CASE` statements, `REPEAT` loops
- User-defined `FUNCTION_BLOCK`s and `FUNCTION`s, and calls to them
- Arrays and structured (`STRUCT`) types
- Additional elementary types: `SINT/USINT/UINT/UDINT/LINT/ULINT/WORD/DWORD/LWORD`
- Multiple POUs scheduled at different rates/priorities within one engine
  (`ScanEngine` currently hosts exactly one top-level `IProgram`)

## Phase 2 and beyond

- **Ladder Logic (LD) and Function Block Diagram (FBD)**: implement as
  additional `IProgram`s (or additional front-ends compiling down to the
  existing ST AST/interpreter), reusing the scan engine, tag database, and
  I/O abstraction unchanged.
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
- **Expanded ST language surface**: `FOR`, `CASE`, function blocks/functions,
  arrays/structs, remaining elementary types.
- **Persistence**: retentive (`VAR RETAIN`) variables surviving a restart.
- **HMI/Web UI**: a way to observe/force tag values without recompiling a
  program (out of scope until the above runtime pieces are solid).
