import { VarDeclList } from './VarDeclList'
import { InstanceList } from './InstanceList'
import { NetworkList } from './NetworkList'
import type { BlockDef, Project } from './types'

// Editor for one selected block. Which sections appear depends on its kind, mirroring
// the real IEC POU rules this compiles against (see docs/architecture.md): a Function
// has no plain VAR or FB instances (it's stateless), only Main/CyclicInterrupt/
// FunctionBlock get a "VAR" section and can declare instances.
export function BlockEditor({
  project,
  block,
  onChange,
}: {
  project: Project
  block: BlockDef
  onChange: (block: BlockDef) => void
}) {
  const availableInstanceTypes = project.blocks
    .filter((b) => b.kind === 'FunctionBlock' && b.name !== block.name)
    .map((b) => b.name)

  const availableCallees = [
    ...(block.instances ?? []).map((i) => i.name),
    ...project.blocks.filter((b) => b.kind === 'Function' && b.name !== block.name).map((b) => b.name),
  ]

  const hasPlainVars =
    block.kind === 'Main' || block.kind === 'CyclicInterrupt' || block.kind === 'FunctionBlock'
  const hasInOut = block.kind === 'FunctionBlock' || block.kind === 'Function'
  // VAR_TEMP is only valid inside a FUNCTION_BLOCK/FUNCTION POU (see
  // Parser::parsePouDef) -- a top-level PROGRAM's grammar has no VAR_TEMP keyword at
  // all, and Main/CyclicInterrupt both compile down into a PROGRAM (CyclicInterrupt's
  // body is inlined into Main's, see compile.ts), so neither can have one.
  const hasVarTemp = hasInOut

  return (
    <div className="block-editor">
      <div className="block-editor__header">
        <label>
          Name
          <input value={block.name} onChange={(e) => onChange({ ...block, name: e.target.value })} />
        </label>
        <span className="block-editor__kind">{block.kind}</span>
        {block.kind === 'CyclicInterrupt' && (
          <label>
            Interval (ms)
            <input
              type="number"
              min={1}
              value={block.intervalMs ?? 100}
              onChange={(e) => onChange({ ...block, intervalMs: Number(e.target.value) })}
            />
          </label>
        )}
      </div>

      {hasInOut && (
        <>
          <VarDeclList
            label="VAR_INPUT"
            decls={block.varInput ?? []}
            onChange={(decls) => onChange({ ...block, varInput: decls })}
          />
          <VarDeclList
            label="VAR_OUTPUT"
            decls={block.varOutput ?? []}
            onChange={(decls) => onChange({ ...block, varOutput: decls })}
          />
        </>
      )}

      {hasPlainVars && (
        <>
          <VarDeclList
            label="VAR"
            decls={block.vars ?? []}
            onChange={(decls) => onChange({ ...block, vars: decls })}
          />
          <InstanceList
            instances={block.instances ?? []}
            availableTypes={availableInstanceTypes}
            onChange={(instances) => onChange({ ...block, instances })}
          />
        </>
      )}

      {hasVarTemp && (
        <VarDeclList
          label="VAR_TEMP"
          decls={block.varTemp ?? []}
          onChange={(decls) => onChange({ ...block, varTemp: decls })}
        />
      )}

      <h3>Networks</h3>
      <NetworkList
        networks={block.networks}
        availableCallees={availableCallees}
        onChange={(networks) => onChange({ ...block, networks })}
      />
    </div>
  )
}
