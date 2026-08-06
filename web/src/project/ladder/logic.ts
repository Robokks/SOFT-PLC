import { appendItem, removeAt, replaceAt } from '../edit'
import type { ContactElement, ParallelElement, RungLogic, RungSegment } from '../types'

// Turns structured rung logic into the same boolean-expression text RungStmt already
// parses (RUNG's grammar: series contacts are AND, parallel branches are OR, a
// normally-closed contact is `NOT tag`) -- see docs/architecture.md's "Ladder
// Diagram" section. This is the ONLY place that text gets produced; compile.ts never
// needs to know `rung` exists, because whoever edits `rung` also writes the derived
// string into `NetworkDef.condition` (see NetworkEditor's onRungChange).
//
// No parens are needed around an AND-chain (there are none in this simplified model
// -- see ParallelElement's comment in types.ts), but a parallel (OR) segment must be
// parenthesized whenever it's combined in series with anything else: RUNG's
// precedence has AND bind tighter than OR (see docs/architecture.md), so
// `Start OR Motor AND NOT Stop` would parse as `Start OR (Motor AND NOT Stop)` --
// wrong -- while `(Start OR Motor) AND NOT Stop` is the intended seal-in circuit.

function contactExpr(c: ContactElement): string {
  return c.negate ? `NOT ${c.tag}` : c.tag
}

function segmentExpr(seg: RungSegment): string {
  if (seg.kind === 'contact') {
    return contactExpr(seg)
  }
  if (seg.branches.length === 0) {
    return ''
  }
  const branchStrs = seg.branches.map(contactExpr)
  return branchStrs.length === 1 ? branchStrs[0] : `(${branchStrs.join(' OR ')})`
}

export function rungLogicToCondition(logic: RungLogic): string {
  if (logic.segments.length === 0) {
    return 'TRUE'
  }
  return logic.segments.map(segmentExpr).join(' AND ')
}

export function emptyRungLogic(): RungLogic {
  return { segments: [{ kind: 'contact', negate: false, tag: '' }] }
}

export function addSeriesContact(logic: RungLogic): RungLogic {
  return { segments: appendItem(logic.segments, { kind: 'contact', negate: false, tag: '' }) }
}

export function removeSegment(logic: RungLogic, index: number): RungLogic {
  return { segments: removeAt(logic.segments, index) }
}

export function updateSegment(logic: RungLogic, index: number, segment: RungSegment): RungLogic {
  return { segments: replaceAt(logic.segments, index, segment) }
}

// "+ OR branch" on a plain contact: turns it into a 2-branch parallel group (itself
// plus a fresh empty branch) without losing what was already there.
export function makeParallel(segment: RungSegment): ParallelElement {
  if (segment.kind === 'parallel') {
    return {
      kind: 'parallel',
      branches: appendItem(segment.branches, { kind: 'contact', negate: false, tag: '' }),
    }
  }
  return { kind: 'parallel', branches: [segment, { kind: 'contact', negate: false, tag: '' }] }
}

// Removing a branch down to exactly one collapses the parallel group back into a
// plain contact, since a 1-branch OR has no reason to keep the extra structure
// around (and RungDiagram only ever wants to show real branching, not a group of
// one).
export function removeBranch(segment: ParallelElement, branchIndex: number): RungSegment {
  const branches = removeAt(segment.branches, branchIndex)
  return branches.length === 1 ? branches[0] : { kind: 'parallel', branches }
}
