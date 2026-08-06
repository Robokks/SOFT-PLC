import { describe, expect, it, vi } from 'vitest'
import { fireEvent, render, screen } from '@testing-library/react'
import { RungDiagram } from './RungDiagram'
import { rungLogicToCondition } from './logic'
import type { CoilOutput, RungLogic } from '../types'

describe('RungDiagram', () => {
  it('renders a contact and a coil as SVG symbols with their tag names', () => {
    const rung: RungLogic = { segments: [{ kind: 'contact', negate: false, tag: 'Start' }] }
    const outputs: CoilOutput[] = [{ kind: 'Direct', target: 'Motor' }]
    render(<RungDiagram rung={rung} outputs={outputs} onRungChange={vi.fn()} onOutputsChange={vi.fn()} />)

    expect(screen.getByRole('img', { name: /ladder rung/i })).toBeInTheDocument()
    // The tag/target names are rendered as SVG <text>, findable by their content.
    expect(screen.getByText('Start')).toBeInTheDocument()
    expect(screen.getByText('Motor')).toBeInTheDocument()
  })

  it('editing a contact tag calls onRungChange with the updated contact', () => {
    const rung: RungLogic = { segments: [{ kind: 'contact', negate: false, tag: '' }] }
    const onRungChange = vi.fn()
    render(<RungDiagram rung={rung} outputs={[]} onRungChange={onRungChange} onOutputsChange={vi.fn()} />)

    fireEvent.change(screen.getByPlaceholderText('Tag'), { target: { value: 'Start' } })

    expect(onRungChange).toHaveBeenCalledWith({
      segments: [{ kind: 'contact', negate: false, tag: 'Start' }],
    })
  })

  it('toggling NO/NC sets negate', () => {
    const rung: RungLogic = { segments: [{ kind: 'contact', negate: false, tag: 'Stop' }] }
    const onRungChange = vi.fn()
    render(<RungDiagram rung={rung} outputs={[]} onRungChange={onRungChange} onOutputsChange={vi.fn()} />)

    fireEvent.change(screen.getByRole('combobox'), { target: { value: 'NC' } })

    expect(onRungChange).toHaveBeenCalledWith({
      segments: [{ kind: 'contact', negate: true, tag: 'Stop' }],
    })
  })

  it('"+ OR branch" turns a contact into a parallel group, then "+ Series contact" builds the full seal-in circuit', () => {
    let rung: RungLogic = { segments: [{ kind: 'contact', negate: false, tag: 'Start' }] }
    const { rerender } = render(
      <RungDiagram
        rung={rung}
        outputs={[]}
        onRungChange={(r) => {
          rung = r
        }}
        onOutputsChange={vi.fn()}
      />,
    )

    fireEvent.click(screen.getByRole('button', { name: '+ OR branch' }))
    expect(rung.segments).toEqual([
      {
        kind: 'parallel',
        branches: [
          { kind: 'contact', negate: false, tag: 'Start' },
          { kind: 'contact', negate: false, tag: '' },
        ],
      },
    ])
    rerender(
      <RungDiagram rung={rung} outputs={[]} onRungChange={(r) => (rung = r)} onOutputsChange={vi.fn()} />,
    )

    // Fill the new branch's tag.
    const tagInputs = screen.getAllByPlaceholderText('Tag')
    fireEvent.change(tagInputs[1], { target: { value: 'Motor' } })
    rerender(
      <RungDiagram rung={rung} outputs={[]} onRungChange={(r) => (rung = r)} onOutputsChange={vi.fn()} />,
    )

    // Add a series contact after the OR group and make it NOT Stop.
    fireEvent.click(screen.getByRole('button', { name: '+ Series contact' }))
    rerender(
      <RungDiagram rung={rung} outputs={[]} onRungChange={(r) => (rung = r)} onOutputsChange={vi.fn()} />,
    )
    fireEvent.change(screen.getAllByPlaceholderText('Tag')[2], { target: { value: 'Stop' } })
    rerender(
      <RungDiagram rung={rung} outputs={[]} onRungChange={(r) => (rung = r)} onOutputsChange={vi.fn()} />,
    )
    const ncSelects = screen.getAllByRole('combobox')
    fireEvent.change(ncSelects[ncSelects.length - 1], { target: { value: 'NC' } })

    expect(rungLogicToCondition(rung)).toBe('(Start OR Motor) AND NOT Stop')
  })

  it('coil controls add/edit/remove outputs', () => {
    let outputs: CoilOutput[] = []
    const rung: RungLogic = { segments: [] }
    const { rerender } = render(
      <RungDiagram rung={rung} outputs={outputs} onRungChange={vi.fn()} onOutputsChange={(o) => (outputs = o)} />,
    )

    fireEvent.click(screen.getByRole('button', { name: '+ Add coil' }))
    expect(outputs).toEqual([{ kind: 'Direct', target: '' }])
    rerender(
      <RungDiagram rung={rung} outputs={outputs} onRungChange={vi.fn()} onOutputsChange={(o) => (outputs = o)} />,
    )

    fireEvent.change(screen.getByPlaceholderText('Target tag'), { target: { value: 'Motor' } })
    expect(outputs).toEqual([{ kind: 'Direct', target: 'Motor' }])
    rerender(
      <RungDiagram rung={rung} outputs={outputs} onRungChange={vi.fn()} onOutputsChange={(o) => (outputs = o)} />,
    )

    fireEvent.click(screen.getByRole('button', { name: 'Remove coil' }))
    expect(outputs).toEqual([])
  })
})
