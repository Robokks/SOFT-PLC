import type { StatusInfo } from '../types'

export function StatusBar({ status }: { status: StatusInfo | null }) {
  if (!status) {
    return <div className="status-bar status-bar--unknown">connecting…</div>
  }

  return (
    <div className={`status-bar ${status.running ? 'status-bar--running' : 'status-bar--stopped'}`}>
      <span className="status-bar__state">{status.running ? 'RUN' : 'STOP'}</span>
      <span>{status.programName || '(no program loaded)'}</span>
      <span>{status.tagCount} tags</span>
      {status.diagnostics && (
        <span>
          {status.diagnostics.cycleCount} cycles, {status.diagnostics.overrunCount} overruns,
          last {(status.diagnostics.lastCycleDurationUs / 1000).toFixed(2)}ms
        </span>
      )}
    </div>
  )
}
