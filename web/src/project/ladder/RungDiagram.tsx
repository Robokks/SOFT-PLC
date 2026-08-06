import type { CoilKind, CoilOutput, ContactElement, RungLogic, RungSegment } from '../types'
import { appendItem, removeAt, replaceAt } from '../edit'
import { addSeriesContact, makeParallel, removeBranch, removeSegment, updateSegment } from './logic'

const COL_W = 130
const ROW_H = 56
const RAIL_MARGIN = 24
const SEGMENTS_START = RAIL_MARGIN + 30
const TICK_HALF = 12

function contactRows(seg: RungSegment): number {
  return seg.kind === 'parallel' ? Math.max(1, seg.branches.length) : 1
}

// Real ladder symbols, simplified: two ticks for a contact (a diagonal slash added
// for normally-closed), a rounded "pill" for a coil (a plain rectangle stands in for
// the traditional double-arc "( )" -- visually distinct and much simpler to draw
// correctly than bezier arcs, at the cost of IEC-symbol fidelity).
function ContactSymbol({ x, y, contact }: { x: number; y: number; contact: ContactElement }) {
  return (
    <g className="rung-diagram__contact">
      <line x1={x - TICK_HALF} y1={y - 10} x2={x - TICK_HALF} y2={y + 10} />
      <line x1={x + TICK_HALF} y1={y - 10} x2={x + TICK_HALF} y2={y + 10} />
      {contact.negate && <line x1={x - TICK_HALF - 5} y1={y + 13} x2={x + TICK_HALF + 5} y2={y - 13} />}
      <text x={x} y={y - 18} textAnchor="middle">
        {contact.tag || '?'}
      </text>
    </g>
  )
}

function CoilSymbol({ x, y, kind, target }: { x: number; y: number; kind: CoilKind; target: string }) {
  return (
    <g className="rung-diagram__coil">
      <rect x={x - 16} y={y - 12} width={32} height={24} rx={12} />
      {kind !== 'Direct' && (
        <text x={x} y={y + 4} textAnchor="middle" className="rung-diagram__coil-kind">
          {kind === 'Set' ? 'S' : 'R'}
        </text>
      )}
      <text x={x} y={y - 18} textAnchor="middle">
        {target || '?'}
      </text>
    </g>
  )
}

// The visual rung itself: two rails, a wire running left to right through each
// segment (a plain contact sits on the main wire; a parallel group ties the main
// wire down into N stacked branch wires and back up), ending in one or more coils.
// Purely a rendering of `rung`/`outputs` -- editing happens in the control strip
// below, which is what actually calls onRungChange/onOutputsChange; this half never
// mutates anything itself.
function RungSvg({ rung, outputs }: { rung: RungLogic; outputs: CoilOutput[] }) {
  const segments = rung.segments
  const rows = Math.max(1, ...segments.map(contactRows), outputs.length)
  const width = SEGMENTS_START + segments.length * COL_W + COL_W + RAIL_MARGIN
  const height = (rows - 1) * ROW_H + 60
  const mainY = 30

  const segX = (i: number) => SEGMENTS_START + i * COL_W
  const coilColX = segX(segments.length) + 65

  return (
    <svg
      width={width}
      height={height}
      viewBox={`0 0 ${width} ${height}`}
      className="rung-diagram__svg"
      role="img"
      aria-label="Ladder rung diagram"
    >
      <line className="rung-diagram__rail" x1={RAIL_MARGIN} y1={0} x2={RAIL_MARGIN} y2={height} />
      <line className="rung-diagram__rail" x1={width - RAIL_MARGIN} y1={0} x2={width - RAIL_MARGIN} y2={height} />

      <line className="rung-diagram__wire" x1={RAIL_MARGIN} y1={mainY} x2={segX(0)} y2={mainY} />

      {segments.map((seg, i) => {
        const xStart = segX(i)
        const xEnd = segX(i + 1)
        const xCenter = (xStart + xEnd) / 2
        if (seg.kind === 'contact') {
          return (
            <g key={i}>
              <line className="rung-diagram__wire" x1={xStart} y1={mainY} x2={xEnd} y2={mainY} />
              <ContactSymbol x={xCenter} y={mainY} contact={seg} />
            </g>
          )
        }
        const lastY = mainY + (seg.branches.length - 1) * ROW_H
        return (
          <g key={i}>
            <line className="rung-diagram__wire" x1={xStart} y1={mainY} x2={xStart} y2={lastY} />
            <line className="rung-diagram__wire" x1={xEnd} y1={mainY} x2={xEnd} y2={lastY} />
            {seg.branches.map((c, bi) => {
              const y = mainY + bi * ROW_H
              return (
                <g key={bi}>
                  <line className="rung-diagram__wire" x1={xStart} y1={y} x2={xEnd} y2={y} />
                  <ContactSymbol x={xCenter} y={y} contact={c} />
                </g>
              )
            })}
          </g>
        )
      })}

      <line
        className="rung-diagram__wire"
        x1={segX(segments.length)}
        y1={mainY}
        x2={coilColX - 30}
        y2={mainY}
      />

      {outputs.length > 1 && (
        <>
          <line
            className="rung-diagram__wire"
            x1={coilColX - 30}
            y1={mainY}
            x2={coilColX - 30}
            y2={mainY + (outputs.length - 1) * ROW_H}
          />
          <line
            className="rung-diagram__wire"
            x1={width - RAIL_MARGIN}
            y1={mainY}
            x2={width - RAIL_MARGIN}
            y2={mainY + (outputs.length - 1) * ROW_H}
          />
        </>
      )}
      {(outputs.length > 0 ? outputs : [null]).map((o, i) => {
        const y = mainY + i * ROW_H
        return (
          <g key={i}>
            <line className="rung-diagram__wire" x1={coilColX - 30} y1={y} x2={coilColX - 16} y2={y} />
            {o && <CoilSymbol x={coilColX} y={y} kind={o.kind} target={o.target} />}
            <line
              className="rung-diagram__wire"
              x1={coilColX + (o ? 16 : -30)}
              y1={y}
              x2={width - RAIL_MARGIN}
              y2={y}
            />
          </g>
        )
      })}
    </svg>
  )
}

function ContactFields({
  contact,
  onChange,
}: {
  contact: ContactElement
  onChange: (c: ContactElement) => void
}) {
  return (
    <span className="contact-fields">
      <select
        value={contact.negate ? 'NC' : 'NO'}
        onChange={(e) => onChange({ ...contact, negate: e.target.value === 'NC' })}
      >
        <option value="NO">NO</option>
        <option value="NC">NC</option>
      </select>
      <input placeholder="Tag" value={contact.tag} onChange={(e) => onChange({ ...contact, tag: e.target.value })} />
    </span>
  )
}

function SegmentControls({
  segment,
  onChange,
  onRemove,
}: {
  segment: RungSegment
  onChange: (s: RungSegment) => void
  onRemove: () => void
}) {
  if (segment.kind === 'contact') {
    return (
      <div className="segment-controls">
        <ContactFields contact={segment} onChange={onChange} />
        <button onClick={() => onChange(makeParallel(segment))}>+ OR branch</button>
        <button onClick={onRemove}>Remove</button>
      </div>
    )
  }
  return (
    <div className="segment-controls segment-controls--parallel">
      {segment.branches.map((b, bi) => (
        <div className="segment-controls__branch" key={bi}>
          <ContactFields
            contact={b}
            onChange={(c) => onChange({ ...segment, branches: replaceAt(segment.branches, bi, c) })}
          />
          <button onClick={() => onChange(removeBranch(segment, bi))}>Remove branch</button>
        </div>
      ))}
      <button onClick={() => onChange(makeParallel(segment))}>+ OR branch</button>
      <button onClick={onRemove}>Remove segment</button>
    </div>
  )
}

function CoilControls({
  outputs,
  onChange,
}: {
  outputs: CoilOutput[]
  onChange: (outputs: CoilOutput[]) => void
}) {
  return (
    <div className="coil-controls">
      {outputs.map((o, i) => (
        <span className="coil-controls__item" key={i}>
          <select
            value={o.kind}
            onChange={(e) => onChange(replaceAt(outputs, i, { ...o, kind: e.target.value as CoilKind }))}
          >
            <option value="Direct">Direct</option>
            <option value="Set">Set</option>
            <option value="Reset">Reset</option>
          </select>
          <input
            placeholder="Target tag"
            value={o.target}
            onChange={(e) => onChange(replaceAt(outputs, i, { ...o, target: e.target.value }))}
          />
          <button onClick={() => onChange(removeAt(outputs, i))}>Remove coil</button>
        </span>
      ))}
      <button onClick={() => onChange(appendItem(outputs, { kind: 'Direct', target: '' }))}>+ Add coil</button>
    </div>
  )
}

// A real Ladder rung: rendered live from `rung`/`outputs` (RungSvg, read-only) with
// an editing strip below it (SegmentControls/CoilControls) in the same left-to-right
// order as the diagram, so it's clear which control drives which symbol. Every edit
// recomputes and hands back the derived condition text too (rungLogicToCondition),
// via the caller's onRungChange -- see NetworkList.tsx's NetworkEditor, which is the
// only thing that actually writes NetworkDef.condition from this.
export function RungDiagram({
  rung,
  outputs,
  onRungChange,
  onOutputsChange,
}: {
  rung: RungLogic
  outputs: CoilOutput[]
  onRungChange: (rung: RungLogic) => void
  onOutputsChange: (outputs: CoilOutput[]) => void
}) {
  return (
    <div className="rung-diagram">
      <RungSvg rung={rung} outputs={outputs} />
      <div className="rung-diagram__controls">
        {rung.segments.map((seg, i) => (
          <SegmentControls
            key={i}
            segment={seg}
            onChange={(s) => onRungChange(updateSegment(rung, i, s))}
            onRemove={() => onRungChange(removeSegment(rung, i))}
          />
        ))}
        <button onClick={() => onRungChange(addSeriesContact(rung))}>+ Series contact</button>
      </div>
      <CoilControls outputs={outputs} onChange={onOutputsChange} />
    </div>
  )
}
