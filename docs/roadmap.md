# Roadmap

## Phase 1 (this repository, current state)

Core scan-cycle engine, thread-safe tag database, simulated I/O, and a real
minimal Structured Text pipeline. See `docs/architecture.md`.

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
- **Real I/O drivers**: Modbus (TCP/RTU), GPIO (e.g. Raspberry Pi), EtherCAT
  — all implementing `IIoDriver`, requiring no `ScanEngine` changes.
- **External tag access**: OPC-UA and/or Modbus server exposing `TagStore`
  to HMI/SCADA clients, exercising the `shared_mutex` concurrent-reader path
  the store was designed for.
- **Expanded ST language surface**: `FOR`, `CASE`, function blocks/functions,
  arrays/structs, remaining elementary types.
- **Persistence**: retentive (`VAR RETAIN`) variables surviving a restart.
- **HMI/Web UI**: a way to observe/force tag values without recompiling a
  program (out of scope until the above runtime pieces are solid).
