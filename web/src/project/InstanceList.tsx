import type { InstanceDecl } from './types'
import { appendItem, removeAt, replaceAt } from './edit'

// An editable table of FB instance declarations. `availableTypes` (the project's own
// FunctionBlock-kind block names) drives a dropdown rather than a free-text field, so
// an instance can't be typo'd into pointing at a type that doesn't exist.
export function InstanceList({
  instances,
  availableTypes,
  onChange,
}: {
  instances: InstanceDecl[]
  availableTypes: string[]
  onChange: (instances: InstanceDecl[]) => void
}) {
  function update(index: number, patch: Partial<InstanceDecl>) {
    onChange(replaceAt(instances, index, { ...instances[index], ...patch }))
  }

  return (
    <div className="instance-list">
      <div className="instance-list__label">FB Instances</div>
      <table>
        <thead>
          <tr>
            <th>Instance name</th>
            <th>Type</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {instances.map((inst, i) => (
            <tr key={i}>
              <td>
                <input value={inst.name} onChange={(e) => update(i, { name: e.target.value })} />
              </td>
              <td>
                <select value={inst.typeName} onChange={(e) => update(i, { typeName: e.target.value })}>
                  <option value="" disabled>
                    (choose a Function Block)
                  </option>
                  {availableTypes.map((t) => (
                    <option key={t} value={t}>
                      {t}
                    </option>
                  ))}
                </select>
              </td>
              <td>
                <button onClick={() => onChange(removeAt(instances, i))}>Remove</button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <button
        onClick={() =>
          onChange(appendItem(instances, { name: '', typeName: availableTypes[0] ?? '' }))
        }
      >
        + Add instance
      </button>
    </div>
  )
}
