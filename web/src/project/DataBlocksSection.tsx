import { VarDeclList } from './VarDeclList'
import { appendItem, removeAt, replaceAt } from './edit'
import type { DataBlockDef, Project } from './types'

// DB creation: a named list of members, each declared into TagStore as
// "<DBName>.<Member>" once compiled (see docs/architecture.md's "Data Blocks..."
// section) -- a persistent named record with no code, reusing VarDeclList exactly as
// a block's own VAR section does.
export function DataBlocksSection({
  project,
  onChange,
}: {
  project: Project
  onChange: (project: Project) => void
}) {
  function update(index: number, db: DataBlockDef) {
    onChange({ ...project, dataBlocks: replaceAt(project.dataBlocks, index, db) })
  }

  function addDataBlock() {
    const existing = new Set(project.dataBlocks.map((d) => d.name))
    let name = 'DB1'
    let n = 1
    while (existing.has(name)) {
      name = `DB${++n}`
    }
    onChange({ ...project, dataBlocks: appendItem(project.dataBlocks, { name, members: [] }) })
  }

  return (
    <div className="data-blocks-section">
      {project.dataBlocks.map((db, i) => (
        <div className="data-blocks-section__block" key={i}>
          <div className="data-blocks-section__header">
            <label>
              Name
              <input value={db.name} onChange={(e) => update(i, { ...db, name: e.target.value })} />
            </label>
            <button onClick={() => onChange({ ...project, dataBlocks: removeAt(project.dataBlocks, i) })}>
              Remove DB
            </button>
          </div>
          <VarDeclList
            label="Members"
            decls={db.members}
            onChange={(members) => update(i, { ...db, members })}
          />
        </div>
      ))}
      <button onClick={addDataBlock}>+ Add Data Block</button>
    </div>
  )
}
