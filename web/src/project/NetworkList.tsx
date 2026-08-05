import type { CallArg, CallDef, CoilKind, CoilOutput, NetworkDef } from './types'
import { appendItem, removeAt, replaceAt } from './edit'

const kCoilKinds: CoilKind[] = ['Direct', 'Set', 'Reset']

function CallEditor({
  call,
  availableCallees,
  onChange,
  onRemove,
}: {
  call: CallDef
  availableCallees: string[]
  onChange: (call: CallDef) => void
  onRemove: () => void
}) {
  function updateArg(index: number, patch: Partial<CallArg>) {
    onChange({ ...call, args: replaceAt(call.args, index, { ...call.args[index], ...patch }) })
  }

  return (
    <div className="call-editor">
      <select value={call.calleeName} onChange={(e) => onChange({ ...call, calleeName: e.target.value })}>
        <option value="" disabled>
          (choose an instance or Function)
        </option>
        {availableCallees.map((c) => (
          <option key={c} value={c}>
            {c}
          </option>
        ))}
      </select>
      <table>
        <thead>
          <tr>
            <th>Param</th>
            <th>Direction</th>
            <th>{'Expr (:=) / Target (=>)'}</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {call.args.map((arg, i) => (
            <tr key={i}>
              <td>
                <input value={arg.param} onChange={(e) => updateArg(i, { param: e.target.value })} />
              </td>
              <td>
                <select
                  value={arg.direction}
                  onChange={(e) => updateArg(i, { direction: e.target.value as 'in' | 'out' })}
                >
                  <option value="in">in (:=)</option>
                  <option value="out">out (=&gt;)</option>
                </select>
              </td>
              <td>
                <input value={arg.expr} onChange={(e) => updateArg(i, { expr: e.target.value })} />
              </td>
              <td>
                <button onClick={() => onChange({ ...call, args: removeAt(call.args, i) })}>
                  Remove
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <button
        onClick={() =>
          onChange({ ...call, args: appendItem(call.args, { param: '', direction: 'in', expr: '' }) })
        }
      >
        + Add arg
      </button>
      <button className="call-editor__remove" onClick={onRemove}>
        Remove this call
      </button>
    </div>
  )
}

function CoilEditor({
  outputs,
  onChange,
}: {
  outputs: CoilOutput[]
  onChange: (outputs: CoilOutput[]) => void
}) {
  function update(index: number, patch: Partial<CoilOutput>) {
    onChange(replaceAt(outputs, index, { ...outputs[index], ...patch }))
  }
  return (
    <table>
      <thead>
        <tr>
          <th>Kind</th>
          <th>Target</th>
          <th />
        </tr>
      </thead>
      <tbody>
        {outputs.map((o, i) => (
          <tr key={i}>
            <td>
              <select value={o.kind} onChange={(e) => update(i, { kind: e.target.value as CoilKind })}>
                {kCoilKinds.map((k) => (
                  <option key={k} value={k}>
                    {k}
                  </option>
                ))}
              </select>
            </td>
            <td>
              <input
                placeholder="Target tag"
                value={o.target}
                onChange={(e) => update(i, { target: e.target.value })}
              />
            </td>
            <td>
              <button onClick={() => onChange(removeAt(outputs, i))}>Remove</button>
            </td>
          </tr>
        ))}
      </tbody>
      <tfoot>
        <tr>
          <td colSpan={3}>
            <button onClick={() => onChange(appendItem(outputs, { kind: 'Direct', target: '' }))}>
              + Add coil
            </button>
          </td>
        </tr>
      </tfoot>
    </table>
  )
}

// One network: an optional title/comment, a row of FB/FC "boxes" (calls) executed
// before the rung, a boolean condition (the same series/parallel/NOT contact grammar
// RUNG already reuses -- typed as plain text here, e.g. "(Start OR Motor) AND NOT
// Stop"), and one or more coils. See docs/architecture.md's "Ladder Diagram" section
// for why this shape maps directly onto RungStmt/CallStmt with no new AST needed.
function NetworkEditor({
  network,
  availableCallees,
  onChange,
  onRemove,
}: {
  network: NetworkDef
  availableCallees: string[]
  onChange: (network: NetworkDef) => void
  onRemove: () => void
}) {
  return (
    <div className="network-editor">
      <div className="network-editor__header">
        <input
          placeholder="Network title"
          value={network.title ?? ''}
          onChange={(e) => onChange({ ...network, title: e.target.value || undefined })}
        />
        <button className="network-editor__remove" onClick={onRemove}>
          Remove network
        </button>
      </div>
      <input
        placeholder="Comment"
        value={network.comment ?? ''}
        onChange={(e) => onChange({ ...network, comment: e.target.value || undefined })}
      />

      <div className="network-editor__calls">
        {network.calls.map((call, i) => (
          <CallEditor
            key={i}
            call={call}
            availableCallees={availableCallees}
            onChange={(c) => onChange({ ...network, calls: replaceAt(network.calls, i, c) })}
            onRemove={() => onChange({ ...network, calls: removeAt(network.calls, i) })}
          />
        ))}
        <button
          onClick={() =>
            onChange({
              ...network,
              calls: appendItem(network.calls, { calleeName: availableCallees[0] ?? '', args: [] }),
            })
          }
        >
          + Add FB/FC call
        </button>
      </div>

      <label className="network-editor__condition">
        Condition
        <input
          placeholder="(Start OR Motor) AND NOT Stop"
          value={network.condition}
          onChange={(e) => onChange({ ...network, condition: e.target.value })}
        />
      </label>

      <CoilEditor
        outputs={network.outputs}
        onChange={(outputs) => onChange({ ...network, outputs })}
      />
    </div>
  )
}

export function NetworkList({
  networks,
  availableCallees,
  onChange,
}: {
  networks: NetworkDef[]
  availableCallees: string[]
  onChange: (networks: NetworkDef[]) => void
}) {
  return (
    <div className="network-list">
      {networks.map((net, i) => (
        <NetworkEditor
          key={i}
          network={net}
          availableCallees={availableCallees}
          onChange={(n) => onChange(replaceAt(networks, i, n))}
          onRemove={() => onChange(removeAt(networks, i))}
        />
      ))}
      <button
        onClick={() => onChange(appendItem(networks, { calls: [], condition: '', outputs: [] }))}
      >
        + Add network
      </button>
    </div>
  )
}
