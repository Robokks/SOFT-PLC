import type { ElementaryType, VarDecl } from './types'
import { appendItem, removeAt, replaceAt } from './edit'

const kElementaryTypes: ElementaryType[] = ['BOOL', 'BYTE', 'INT', 'DINT', 'REAL', 'LREAL', 'TIME', 'STRING']

// An editable table of VarDecl entries -- reused for every VAR/VAR_INPUT/VAR_OUTPUT/
// VAR_TEMP section in BlockEditor, for a DATA_BLOCK's members, and for the project's
// IO-linking list (which is just VarDecl[] with addresses).
export function VarDeclList({
  label,
  decls,
  onChange,
  showAddress = false,
}: {
  label: string
  decls: VarDecl[]
  onChange: (decls: VarDecl[]) => void
  showAddress?: boolean
}) {
  function update(index: number, patch: Partial<VarDecl>) {
    onChange(replaceAt(decls, index, { ...decls[index], ...patch }))
  }

  return (
    <div className="var-decl-list">
      <div className="var-decl-list__label">{label}</div>
      <table>
        <thead>
          <tr>
            <th>Name</th>
            <th>Type</th>
            {showAddress && <th>Address</th>}
            <th>Initial</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {decls.map((decl, i) => (
            <tr key={i}>
              <td>
                <input
                  placeholder="Name"
                  value={decl.name}
                  onChange={(e) => update(i, { name: e.target.value })}
                />
              </td>
              <td>
                <select
                  value={decl.type}
                  onChange={(e) => update(i, { type: e.target.value as ElementaryType })}
                >
                  {kElementaryTypes.map((t) => (
                    <option key={t} value={t}>
                      {t}
                    </option>
                  ))}
                </select>
              </td>
              {showAddress && (
                <td>
                  <input
                    placeholder="%Q0.0"
                    value={decl.address ?? ''}
                    onChange={(e) => update(i, { address: e.target.value || undefined })}
                  />
                </td>
              )}
              <td>
                <input
                  value={decl.initial ?? ''}
                  onChange={(e) => update(i, { initial: e.target.value || undefined })}
                />
              </td>
              <td>
                <button onClick={() => onChange(removeAt(decls, i))}>Remove</button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <button onClick={() => onChange(appendItem(decls, { name: '', type: 'BOOL' }))}>
        + Add {label}
      </button>
    </div>
  )
}
