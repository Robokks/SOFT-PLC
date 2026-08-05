import { describe, expect, it } from 'vitest'
import { compileProject, ProjectCompileError } from './compile'
import type { Project } from './types'
import { emptyProject } from './types'

describe('compileProject', () => {
  it('throws a ProjectCompileError when there is no Main block', () => {
    const project: Project = { name: 'Empty', ioLinking: [], driveConfig: [], dataBlocks: [], blocks: [] }
    expect(() => compileProject(project)).toThrow(ProjectCompileError)
  })

  it('never emits VAR_TEMP for Main, since a top-level PROGRAM has no such keyword', () => {
    const project = emptyProject('P')
    // Even if a hand-edited/imported project sets this (the UI never allows it), the
    // compiler must not emit an invalid PROGRAM ... VAR_TEMP section.
    project.blocks[0].varTemp = [{ name: 'Scratch', type: 'DINT' }]

    const source = compileProject(project)

    expect(source).not.toContain('VAR_TEMP')
  })

  it('compiles IO linking and a rung into Main, matching the blink example shape', () => {
    const project = emptyProject('Blink')
    project.blocks[0].name = 'Blink'
    project.ioLinking = [{ name: 'LedState', type: 'BOOL', address: '%Q0.0', initial: 'FALSE' }]
    project.blocks[0].vars = [
      { name: 'Counter', type: 'DINT', initial: '0' },
      { name: 'ToggleAfterScans', type: 'DINT', initial: '50' },
    ]
    project.blocks[0].networks = [
      {
        title: 'Blink',
        calls: [],
        condition: 'NOT LedState',
        outputs: [{ kind: 'Direct', target: 'LedState' }],
      },
    ]

    const source = compileProject(project)

    expect(source).toContain('PROGRAM Blink')
    expect(source).toContain('LedState AT %Q0.0 : BOOL := FALSE;')
    expect(source).toContain('Counter : DINT := 0;')
    expect(source).toContain('RUNG NOT LedState => LedState;')
    expect(source).toContain('END_PROGRAM')
  })

  it('compiles a DATA_BLOCK', () => {
    const project = emptyProject('P')
    project.dataBlocks = [{ name: 'DB1', members: [{ name: 'Speed', type: 'INT', initial: '0' }] }]

    const source = compileProject(project)

    expect(source).toContain('DATA_BLOCK DB1')
    expect(source).toContain('Speed : INT := 0;')
    expect(source).toContain('END_DATA_BLOCK')
  })

  it('compiles a FunctionBlock and an instance call placed on a network', () => {
    const project = emptyProject('P')
    project.blocks.push({
      name: 'FB_Edge',
      kind: 'FunctionBlock',
      varInput: [{ name: 'CLK', type: 'BOOL' }],
      varOutput: [{ name: 'Q', type: 'BOOL' }],
      vars: [{ name: 'M', type: 'BOOL' }],
      networks: [{ calls: [], condition: 'CLK AND NOT M', outputs: [{ kind: 'Direct', target: 'Q' }] }],
    })
    project.blocks[0].instances = [{ name: 'Edge1', typeName: 'FB_Edge' }]
    project.blocks[0].networks = [
      {
        calls: [{ calleeName: 'Edge1', args: [{ param: 'CLK', direction: 'in', expr: 'Start' }] }],
        condition: 'Edge1.Q',
        outputs: [{ kind: 'Direct', target: 'Motor' }],
      },
    ]

    const source = compileProject(project)

    expect(source).toContain('FUNCTION_BLOCK FB_Edge')
    expect(source).toContain('CLK : BOOL;')
    expect(source).toContain('END_FUNCTION_BLOCK')
    expect(source).toContain('Edge1 : FB_Edge;')
    expect(source).toContain('Edge1(CLK := Start);')
    expect(source).toContain('RUNG Edge1.Q => Motor;')
  })

  it('compiles a Cyclic Interrupt block into an accumulate-and-fire guard in Main', () => {
    const project = emptyProject('P')
    project.blocks.push({
      name: 'FastPoll',
      kind: 'CyclicInterrupt',
      intervalMs: 100,
      vars: [{ name: 'PollCount', type: 'DINT', initial: '0' }],
      networks: [], // networks aren't required for this test; the guard shape is what's checked
    })

    const source = compileProject(project)

    expect(source).toContain('__CyclicAccum_FastPoll : TIME := T#0ms;')
    expect(source).toContain('PollCount : DINT := 0;') // folded into Main's VAR
    expect(source).toContain('IF __CyclicAccum_FastPoll >= T#100ms THEN')
    expect(source).toContain('__CyclicAccum_FastPoll := T#0ms;')
    expect(source).toContain('__CyclicAccum_FastPoll := __CyclicAccum_FastPoll + System.CycleTime;')
  })

  it('emits SET/RESET coil keywords for non-Direct outputs', () => {
    const project = emptyProject('P')
    project.blocks[0].networks = [
      {
        calls: [],
        condition: 'Start',
        outputs: [
          { kind: 'Set', target: 'Motor' },
          { kind: 'Reset', target: 'Fault' },
        ],
      },
    ]

    const source = compileProject(project)

    expect(source).toContain('RUNG Start => SET Motor, RESET Fault;')
  })
})
