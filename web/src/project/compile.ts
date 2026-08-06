import type {
  BlockDef,
  CallArg,
  CallDef,
  CoilOutput,
  DataBlockDef,
  InstanceDecl,
  NetworkDef,
  Project,
  VarDecl,
} from './types'

// Compiles a Project into ST source text that softplc::st::StProgram::load() parses
// completely unchanged -- see docs/architecture.md's "Web frontend" / "Project
// storage" sections for why this compiler lives entirely client-side: every construct
// used below (DATA_BLOCK, FUNCTION_BLOCK/FUNCTION, instance declarations, RUNG,
// CallStmt) already exists in the backend's ST grammar, so there is nothing here that
// needs a new AST node or interpreter change.

function emitVarDecl(v: VarDecl): string {
  const addr = v.address ? ` AT ${v.address}` : ''
  const init = v.initial !== undefined && v.initial !== '' ? ` := ${v.initial}` : ''
  return `        ${v.name}${addr} : ${v.type}${init};`
}

function emitInstanceDecl(inst: InstanceDecl): string {
  return `        ${inst.name} : ${inst.typeName};`
}

function declList(decls: VarDecl[]): string {
  return decls.map(emitVarDecl).join('\n')
}

function instList(insts: InstanceDecl[]): string {
  return insts.map(emitInstanceDecl).join('\n')
}

function varSection(keyword: string, body: string): string {
  return body.trim() === '' ? '' : `    ${keyword}\n${body}\n    END_VAR\n`
}

function emitCallArgs(args: CallArg[]): string {
  return args
    .map((a) => (a.direction === 'in' ? `${a.param} := ${a.expr}` : `${a.param} => ${a.expr}`))
    .join(', ')
}

function emitCall(call: CallDef): string {
  return `    ${call.calleeName}(${emitCallArgs(call.args)});`
}

function emitCoil(o: CoilOutput): string {
  return o.kind === 'Direct' ? o.target : `${o.kind.toUpperCase()} ${o.target}`
}

function emitNetwork(net: NetworkDef): string {
  const lines: string[] = []
  if (net.title || net.comment) {
    lines.push(`    (* ${[net.title, net.comment].filter(Boolean).join(' -- ')} *)`)
  }
  for (const call of net.calls) {
    lines.push(emitCall(call))
  }
  if (net.outputs.length > 0) {
    lines.push(`    RUNG ${net.condition} => ${net.outputs.map(emitCoil).join(', ')};`)
  }
  return lines.join('\n')
}

function emitDataBlock(db: DataBlockDef): string {
  return `DATA_BLOCK ${db.name}\n    VAR\n${declList(db.members)}\n    END_VAR\nEND_DATA_BLOCK\n`
}

// FUNCTION_BLOCK/FUNCTION POUs -- a plain (non-Main, non-CyclicInterrupt) block
// compiles straightforwardly, one VAR_INPUT/VAR_OUTPUT/VAR/VAR_TEMP section each.
function emitPou(block: BlockDef): string {
  const isFunction = block.kind === 'Function'
  const keyword = isFunction ? 'FUNCTION' : 'FUNCTION_BLOCK'
  const endKeyword = isFunction ? 'END_FUNCTION' : 'END_FUNCTION_BLOCK'

  const sections = [
    varSection('VAR_INPUT', declList(block.varInput ?? [])),
    varSection('VAR_OUTPUT', declList(block.varOutput ?? [])),
    varSection(
      'VAR',
      [declList(block.vars ?? []), instList(block.instances ?? [])].filter(Boolean).join('\n'),
    ),
    varSection('VAR_TEMP', declList(block.varTemp ?? [])),
  ].join('')

  const body = block.networks.map(emitNetwork).join('\n')
  return `${keyword} ${block.name}\n${sections}${body}\n${endKeyword}\n`
}

function cyclicAccumName(block: BlockDef): string {
  return `__CyclicAccum_${block.name}`
}

// A Cyclic Interrupt block runs on its own configured interval independent of the
// scan rate in a real PLC. v1 approximates that inline, within Main's own scan,
// rather than as a true separate scheduled task (see docs/architecture.md and
// docs/roadmap.md for why): an elapsed-time accumulator (like TON's own ET) fires the
// block's networks once it reaches the configured interval, then resets to zero --
// exactly the same "accumulate System.CycleTime each scan" pattern
// st/standard_fbs.hpp's TON already uses, just expressed directly in generated ST
// rather than as an FB instance. Known limitation: this block's own vars/instances
// are folded into Main's flat VAR scope (see emitMainVarSections below), not a
// separate scope, so its variable names must not collide with Main's.
function emitCyclicInterruptGuard(block: BlockDef): string {
  const accum = cyclicAccumName(block)
  const body = block.networks.map(emitNetwork).join('\n')
  return [
    `    IF ${accum} >= T#${block.intervalMs ?? 0}ms THEN`,
    body,
    `        ${accum} := T#0ms;`,
    `    ELSE`,
    `        ${accum} := ${accum} + System.CycleTime;`,
    `    END_IF;`,
  ]
    .filter((line) => line.trim() !== '')
    .join('\n')
}

function emitMainVarSections(main: BlockDef, ioLinking: VarDecl[], cyclicBlocks: BlockDef[]): string {
  const accumDecls: VarDecl[] = cyclicBlocks.map((b) => ({
    name: cyclicAccumName(b),
    type: 'TIME',
    initial: 'T#0ms',
  }))
  const plainVars = [
    ...ioLinking,
    ...(main.vars ?? []),
    ...accumDecls,
    ...cyclicBlocks.flatMap((b) => b.vars ?? []),
  ]
  const plainInstances = [...(main.instances ?? []), ...cyclicBlocks.flatMap((b) => b.instances ?? [])]

  // No VAR_TEMP section here: a top-level PROGRAM's grammar has no VAR_TEMP keyword
  // (Parser::parseProgram only loops VAR/VAR_INPUT/VAR_OUTPUT) -- unlike
  // emitPou()'s FUNCTION_BLOCK/FUNCTION sections, which do get one.
  return [
    varSection('VAR_INPUT', declList(main.varInput ?? [])),
    varSection('VAR_OUTPUT', declList(main.varOutput ?? [])),
    varSection('VAR', [declList(plainVars), instList(plainInstances)].filter(Boolean).join('\n')),
  ].join('')
}

export class ProjectCompileError extends Error {}

// Catches incomplete rows (a "+ Add ..." row added and never filled in) before they
// turn into invalid ST that only the backend parser would reject -- and reject with a
// line/column number that means nothing next to a form field. Every check here is a
// UI mistake this exact interaction pattern makes possible, found by driving the real
// app end-to-end and reading its own error output (see docs/architecture.md's
// "Project model and compiler" section): an empty-named IO-linking row silently
// compiled to `    : BOOL;`, surfacing only "expected variable name but found ':'
// (line 9, col 11)" -- correct, but useless to someone who has never seen the
// generated source.
function validateProject(project: Project): void {
  function checkVarDecls(decls: VarDecl[] | undefined, where: string): void {
    (decls ?? []).forEach((d, i) => {
      if (d.name.trim() === '') {
        throw new ProjectCompileError(`${where}: row ${i + 1} has no name.`)
      }
    })
  }
  function checkInstances(instances: InstanceDecl[] | undefined, where: string): void {
    (instances ?? []).forEach((inst, i) => {
      if (inst.name.trim() === '') {
        throw new ProjectCompileError(`${where}: instance row ${i + 1} has no name.`)
      }
      if (inst.typeName.trim() === '') {
        throw new ProjectCompileError(`${where}: instance '${inst.name}' has no type selected.`)
      }
    })
  }
  function checkNetworks(networks: NetworkDef[], where: string): void {
    networks.forEach((net, i) => {
      const label = `${where}, network ${i + 1}${net.title ? ` ('${net.title}')` : ''}`
      net.calls.forEach((call, ci) => {
        if (call.calleeName.trim() === '') {
          throw new ProjectCompileError(`${label}: call ${ci + 1} has no instance/Function selected.`)
        }
        call.args.forEach((arg, ai) => {
          if (arg.param.trim() === '') {
            throw new ProjectCompileError(`${label}: call ${ci + 1}, arg ${ai + 1} has no param name.`)
          }
          if (arg.expr.trim() === '') {
            throw new ProjectCompileError(
              `${label}: call ${ci + 1}, arg '${arg.param}' has no expression/target.`,
            )
          }
        })
      })
      net.outputs.forEach((out, oi) => {
        if (out.target.trim() === '') {
          throw new ProjectCompileError(`${label}: coil ${oi + 1} has no target tag.`)
        }
      })
      if (net.outputs.length > 0 && net.condition.trim() === '') {
        throw new ProjectCompileError(`${label}: has a coil but no condition.`)
      }
    })
  }

  checkVarDecls(project.ioLinking, 'IO Linking')
  for (const db of project.dataBlocks) {
    if (db.name.trim() === '') {
      throw new ProjectCompileError('Data Blocks: a block has no name.')
    }
    checkVarDecls(db.members, `Data Block '${db.name}'`)
  }
  for (const block of project.blocks) {
    if (block.name.trim() === '') {
      throw new ProjectCompileError(`A ${block.kind} block has no name.`)
    }
    checkVarDecls(block.varInput, `Block '${block.name}' VAR_INPUT`)
    checkVarDecls(block.varOutput, `Block '${block.name}' VAR_OUTPUT`)
    checkVarDecls(block.vars, `Block '${block.name}' VAR`)
    checkVarDecls(block.varTemp, `Block '${block.name}' VAR_TEMP`)
    checkInstances(block.instances, `Block '${block.name}'`)
    checkNetworks(block.networks, `Block '${block.name}'`)
  }
}

export function compileProject(project: Project): string {
  const main = project.blocks.find((b) => b.kind === 'Main')
  if (!main) {
    throw new ProjectCompileError('Project has no Main block -- add one before compiling.')
  }
  validateProject(project)

  const cyclicBlocks = project.blocks.filter((b) => b.kind === 'CyclicInterrupt')
  const pouBlocks = project.blocks.filter((b) => b.kind === 'FunctionBlock' || b.kind === 'Function')

  const parts: string[] = []
  for (const db of project.dataBlocks) {
    parts.push(emitDataBlock(db))
  }
  for (const pou of pouBlocks) {
    parts.push(emitPou(pou))
  }

  const mainVarSections = emitMainVarSections(main, project.ioLinking, cyclicBlocks)
  const mainBody = [...main.networks.map(emitNetwork), ...cyclicBlocks.map(emitCyclicInterruptGuard)]
    .filter((s) => s.trim() !== '')
    .join('\n')

  parts.push(`PROGRAM ${main.name}\n${mainVarSections}${mainBody}\nEND_PROGRAM\n`)

  return parts.join('\n')
}
