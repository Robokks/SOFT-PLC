// Mirrors the JSON shapes emitted by server/plc_server.cpp -- see
// docs/architecture.md's "Programming/monitoring HTTP server" section.

export type TagValue = boolean | number | string

export interface TagInfo {
  name: string
  type: string
  value: TagValue
  address?: string
}

export interface ScanDiagnosticsInfo {
  cycleCount: number
  overrunCount: number
  lastCycleDurationUs: number
}

export interface StatusInfo {
  running: boolean
  programName: string
  tagCount: number
  diagnostics?: ScanDiagnosticsInfo
}
