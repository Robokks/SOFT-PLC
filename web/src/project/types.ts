// The project data model this app's UI edits, and PlcServer's project-storage
// endpoints persist as an opaque JSON blob (see docs/architecture.md's "Project
// storage" section -- the backend never parses this shape, only the "compiledSource"
// field by convention). compile.ts turns this into ST source text that the existing
// backend parses completely unchanged.

export type ElementaryType = 'BOOL' | 'BYTE' | 'INT' | 'DINT' | 'REAL' | 'LREAL' | 'TIME' | 'STRING'

export interface VarDecl {
  name: string
  type: ElementaryType
  // Literal text exactly as it appears in ST source (e.g. "0", "TRUE", "T#5s",
  // "'hello'") -- not re-typed/validated here, the backend parser is the source of
  // truth for what's a valid literal.
  initial?: string
  // A direct IEC address, e.g. "%Q0.0" -- only meaningful on an IO-linking entry.
  address?: string
}

export interface InstanceDecl {
  name: string
  typeName: string // a FunctionBlock-kind block's name
}

export interface DataBlockDef {
  name: string
  members: VarDecl[]
}

export type CallArgDirection = 'in' | 'out'

export interface CallArg {
  param: string
  direction: CallArgDirection
  // 'in': the input expression text bound with ':='. 'out': the output target's
  // (dotted) name bound with '=>'.
  expr: string
}

// One FB/FC "box" placed on a network -- see docs/architecture.md's "FB/FC boxes on
// a rung": a call statement placed immediately before the network's RUNG.
export interface CallDef {
  calleeName: string // an instance name (FB call) or a Function block's own name (FC call)
  args: CallArg[]
}

export type CoilKind = 'Direct' | 'Set' | 'Reset'

export interface CoilOutput {
  kind: CoilKind
  target: string
}

// One Ladder network: an optional row of FB/FC boxes, then a boolean condition
// (built from the same contact/branch grammar RUNG already reuses -- series contacts
// are AND, parallel branches are OR, a normally-closed contact is NOT tag) driving one
// or more coils.
export interface NetworkDef {
  title?: string
  comment?: string
  calls: CallDef[]
  condition: string
  outputs: CoilOutput[]
}

export type BlockKind = 'Main' | 'CyclicInterrupt' | 'FunctionBlock' | 'Function'

export interface BlockDef {
  name: string
  kind: BlockKind
  // CyclicInterrupt only: how often this block's networks run, independent of the
  // scan rate. Compiled as an inline accumulate-and-fire guard against
  // System.CycleTime (see compile.ts) -- not a true separate scheduled task in v1.
  intervalMs?: number
  varInput?: VarDecl[] // FunctionBlock/Function only
  varOutput?: VarDecl[] // FunctionBlock/Function only
  varTemp?: VarDecl[]
  vars?: VarDecl[] // Main/CyclicInterrupt/FunctionBlock only (not Function)
  instances?: InstanceDecl[] // Main/CyclicInterrupt/FunctionBlock only (not Function)
  networks: NetworkDef[]
}

export type ModbusRegisterType = 'Coil' | 'DiscreteInput' | 'HoldingRegister' | 'InputRegister'

export interface ModbusPointDef {
  tagName: string
  registerType: ModbusRegisterType
  address: number
}

export interface ModbusDeviceDef {
  name: string
  transport: 'tcp' | 'rtu'
  host?: string
  port?: number
  devicePath?: string
  baud?: number
  unitId: number
  points: ModbusPointDef[]
}

export interface Project {
  name: string
  // Tags with a direct IEC address, declared into Main's VAR section on compile --
  // this is the "IO linking" section (see docs/architecture.md).
  ioLinking: VarDecl[]
  // Modbus TCP/RTU device/point configuration (see docs/roadmap.md's "drive
  // configuration" note: this is form-editable now, but plc_server still always runs
  // a SimulatedIoDriver in v1 -- wiring driveConfig into a real IIoDriver is tracked,
  // not yet done).
  driveConfig: ModbusDeviceDef[]
  dataBlocks: DataBlockDef[]
  blocks: BlockDef[]
  // The last successful compileProject() output, saved alongside the project so
  // PlcServer can auto-load it on startup without understanding this schema at all.
  compiledSource?: string
  updatedAt?: string
}

export function emptyProject(name: string): Project {
  return {
    name,
    ioLinking: [],
    driveConfig: [],
    dataBlocks: [],
    blocks: [{ name: 'Main', kind: 'Main', vars: [], instances: [], networks: [] }],
  }
}
