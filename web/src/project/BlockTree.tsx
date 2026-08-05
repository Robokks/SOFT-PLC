import { useState } from 'react'
import { BlockEditor } from './BlockEditor'
import { removeAt, replaceAt } from './edit'
import type { BlockDef, BlockKind, Project } from './types'

const kCreatableKinds: { kind: BlockKind; label: string }[] = [
  { kind: 'CyclicInterrupt', label: '+ Cyclic Interrupt' },
  { kind: 'FunctionBlock', label: '+ Function Block' },
  { kind: 'Function', label: '+ Function' },
]

function newBlock(kind: BlockKind, existingNames: Set<string>): BlockDef {
  let base = kind === 'CyclicInterrupt' ? 'Cyclic' : kind === 'FunctionBlock' ? 'FB' : kind === 'Function' ? 'FC' : 'Main'
  let name = base
  let n = 1
  while (existingNames.has(name)) {
    name = `${base}${++n}`
  }
  const block: BlockDef = { name, kind, networks: [] }
  if (kind === 'CyclicInterrupt') {
    block.intervalMs = 100
    block.vars = []
    block.instances = []
  } else if (kind === 'FunctionBlock') {
    block.varInput = []
    block.varOutput = []
    block.varTemp = []
    block.vars = []
    block.instances = []
  } else if (kind === 'Function') {
    block.varInput = []
    block.varOutput = []
    block.varTemp = []
  }
  return block
}

// The Programming section: a block list (Main, plus any Cyclic Interrupt/Function
// Block/Function blocks the user has added) and the selected block's editor. "Right-
// click to create a block" from the original request is implemented here as a
// button row instead of a context menu -- same functional outcome (create a Main or
// Cyclic Interrupt block, etc.), simpler to build and use.
export function BlockTree({
  project,
  onChange,
}: {
  project: Project
  onChange: (project: Project) => void
}) {
  const [selectedIndex, setSelectedIndex] = useState(0)
  const selected = project.blocks[Math.min(selectedIndex, project.blocks.length - 1)] as
    | BlockDef
    | undefined

  function addBlock(kind: BlockKind) {
    const existingNames = new Set(project.blocks.map((b) => b.name))
    const block = newBlock(kind, existingNames)
    onChange({ ...project, blocks: [...project.blocks, block] })
    setSelectedIndex(project.blocks.length)
  }

  function updateBlock(index: number, block: BlockDef) {
    onChange({ ...project, blocks: replaceAt(project.blocks, index, block) })
  }

  function removeBlock(index: number) {
    onChange({ ...project, blocks: removeAt(project.blocks, index) })
    setSelectedIndex(0)
  }

  return (
    <div className="block-tree">
      <aside className="block-tree__list">
        <ul>
          {project.blocks.map((b, i) => (
            <li key={i}>
              <button
                className={i === selectedIndex ? 'active' : ''}
                onClick={() => setSelectedIndex(i)}
              >
                {b.name} <span className="block-tree__kind">{b.kind}</span>
              </button>
              {b.kind !== 'Main' && (
                <button className="block-tree__remove" onClick={() => removeBlock(i)}>
                  &times;
                </button>
              )}
            </li>
          ))}
        </ul>
        <div className="block-tree__add">
          {kCreatableKinds.map(({ kind, label }) => (
            <button key={kind} onClick={() => addBlock(kind)}>
              {label}
            </button>
          ))}
        </div>
      </aside>
      <div className="block-tree__editor">
        {selected && (
          <BlockEditor
            project={project}
            block={selected}
            onChange={(block) => updateBlock(project.blocks.indexOf(selected), block)}
          />
        )}
      </div>
    </div>
  )
}
