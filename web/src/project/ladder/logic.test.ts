import { describe, expect, it } from 'vitest'
import {
  addSeriesContact,
  emptyRungLogic,
  makeParallel,
  removeBranch,
  removeSegment,
  rungLogicToCondition,
  updateSegment,
} from './logic'
import type { ParallelElement, RungLogic } from '../types'

describe('rungLogicToCondition', () => {
  it('renders a single contact', () => {
    const logic: RungLogic = { segments: [{ kind: 'contact', negate: false, tag: 'Start' }] }
    expect(rungLogicToCondition(logic)).toBe('Start')
  })

  it('negates a normally-closed contact with NOT', () => {
    const logic: RungLogic = { segments: [{ kind: 'contact', negate: true, tag: 'Stop' }] }
    expect(rungLogicToCondition(logic)).toBe('NOT Stop')
  })

  it('ANDs series contacts with no parens needed', () => {
    const logic: RungLogic = {
      segments: [
        { kind: 'contact', negate: false, tag: 'A' },
        { kind: 'contact', negate: false, tag: 'B' },
        { kind: 'contact', negate: true, tag: 'C' },
      ],
    }
    expect(rungLogicToCondition(logic)).toBe('A AND B AND NOT C')
  })

  it('ORs a parallel group (parenthesized -- always safe, even as the only segment)', () => {
    const logic: RungLogic = {
      segments: [
        {
          kind: 'parallel',
          branches: [
            { kind: 'contact', negate: false, tag: 'Start' },
            { kind: 'contact', negate: false, tag: 'Motor' },
          ],
        },
      ],
    }
    expect(rungLogicToCondition(logic)).toBe('(Start OR Motor)')
  })

  it('parenthesizes a parallel group combined in series -- the seal-in circuit', () => {
    const logic: RungLogic = {
      segments: [
        {
          kind: 'parallel',
          branches: [
            { kind: 'contact', negate: false, tag: 'Start' },
            { kind: 'contact', negate: false, tag: 'Motor' },
          ],
        },
        { kind: 'contact', negate: true, tag: 'Stop' },
      ],
    }
    // Exactly the example RUNG statement from docs/architecture.md's "Ladder
    // Diagram" section -- the whole point of this data model.
    expect(rungLogicToCondition(logic)).toBe('(Start OR Motor) AND NOT Stop')
  })

  it('defaults to TRUE for an empty segment list', () => {
    expect(rungLogicToCondition({ segments: [] })).toBe('TRUE')
  })
})

describe('rung logic edit helpers', () => {
  it('addSeriesContact appends an empty contact', () => {
    const logic = emptyRungLogic()
    const next = addSeriesContact(logic)
    expect(next.segments).toHaveLength(2)
    expect(next.segments[1]).toEqual({ kind: 'contact', negate: false, tag: '' })
    // Immutable: the original is untouched.
    expect(logic.segments).toHaveLength(1)
  })

  it('removeSegment and updateSegment act on the right index', () => {
    const logic: RungLogic = {
      segments: [
        { kind: 'contact', negate: false, tag: 'A' },
        { kind: 'contact', negate: false, tag: 'B' },
      ],
    }
    expect(removeSegment(logic, 0).segments).toEqual([{ kind: 'contact', negate: false, tag: 'B' }])
    expect(updateSegment(logic, 1, { kind: 'contact', negate: true, tag: 'B' }).segments[1]).toEqual({
      kind: 'contact',
      negate: true,
      tag: 'B',
    })
  })

  it('makeParallel turns a contact into a 2-branch group preserving it', () => {
    const contact = { kind: 'contact' as const, negate: false, tag: 'Start' }
    const parallel = makeParallel(contact)
    expect(parallel.branches[0]).toEqual(contact)
    expect(parallel.branches).toHaveLength(2)
  })

  it('makeParallel on an existing parallel group appends another branch', () => {
    const group: ParallelElement = {
      kind: 'parallel',
      branches: [{ kind: 'contact', negate: false, tag: 'Start' }],
    }
    const next = makeParallel(group)
    expect(next.branches).toHaveLength(2)
  })

  it('removeBranch collapses a 2-branch group back into a plain contact', () => {
    const group: ParallelElement = {
      kind: 'parallel',
      branches: [
        { kind: 'contact', negate: false, tag: 'Start' },
        { kind: 'contact', negate: false, tag: 'Motor' },
      ],
    }
    expect(removeBranch(group, 1)).toEqual({ kind: 'contact', negate: false, tag: 'Start' })
  })

  it('removeBranch on a 3-branch group stays a parallel group', () => {
    const group: ParallelElement = {
      kind: 'parallel',
      branches: [
        { kind: 'contact', negate: false, tag: 'A' },
        { kind: 'contact', negate: false, tag: 'B' },
        { kind: 'contact', negate: false, tag: 'C' },
      ],
    }
    const next = removeBranch(group, 1)
    expect(next).toEqual({
      kind: 'parallel',
      branches: [
        { kind: 'contact', negate: false, tag: 'A' },
        { kind: 'contact', negate: false, tag: 'C' },
      ],
    })
  })
})
