import { useState } from 'react'
import { describe, expect, it } from 'vitest'
import { fireEvent, render, screen } from '@testing-library/react'
import { BlockTree } from './BlockTree'
import { compileProject } from './compile'
import { emptyProject } from './types'
import type { Project } from './types'

// Mirrors how ProjectView actually drives BlockTree: project lives in state here,
// and onChange re-renders with the updated project, so a sequence of UI interactions
// composes the same way it would in the real app.
function Harness({ onProject }: { onProject: (p: Project) => void }) {
  const [project, setProject] = useState(emptyProject('Line1'))
  return (
    <BlockTree
      project={project}
      onChange={(p) => {
        setProject(p)
        onProject(p)
      }}
    />
  )
}

describe('BlockTree', () => {
  it('always has a Main block that cannot be removed', () => {
    render(<Harness onProject={() => {}} />)
    expect(screen.getByRole('button', { name: /Main/ })).toBeInTheDocument()
    // Main's row has no remove ("x") button, unlike every other block kind.
    expect(screen.queryByRole('button', { name: '×' })).not.toBeInTheDocument()
  })

  it('builds a working Cyclic Interrupt block through UI interactions alone, compiling correctly', () => {
    let latest: Project = emptyProject('Line1')
    render(<Harness onProject={(p) => (latest = p)} />)

    fireEvent.click(screen.getByRole('button', { name: /\+ Cyclic Interrupt/ }))
    // The new block is auto-selected; rename it and set its interval.
    const nameInput = screen.getByDisplayValue('Cyclic')
    fireEvent.change(nameInput, { target: { value: 'FastPoll' } })

    const intervalInput = screen.getByDisplayValue('100')
    fireEvent.change(intervalInput, { target: { value: '250' } })

    // Add a VAR and a network with a condition + coil, using the now-renamed block's
    // own editor (still selected).
    fireEvent.click(screen.getByRole('button', { name: /\+ Add VAR/ }))
    fireEvent.change(screen.getByPlaceholderText('Name'), { target: { value: 'Heartbeat' } })

    fireEvent.click(screen.getByRole('button', { name: /\+ Add network/ }))
    fireEvent.change(screen.getByPlaceholderText(/Start OR Motor/), {
      target: { value: 'NOT Heartbeat' },
    })
    fireEvent.click(screen.getByRole('button', { name: /\+ Add coil/ }))
    fireEvent.change(screen.getByPlaceholderText('Target tag'), { target: { value: 'Heartbeat' } })

    const source = compileProject(latest)
    expect(source).toContain('__CyclicAccum_FastPoll : TIME := T#0ms;')
    expect(source).toContain('Heartbeat : BOOL;')
    expect(source).toContain('IF __CyclicAccum_FastPoll >= T#250ms THEN')
    expect(source).toContain('RUNG NOT Heartbeat => Heartbeat;')
  })

  it('builds the seal-in circuit as a real Ladder diagram through UI interactions alone', () => {
    let latest: Project = emptyProject('Line1')
    render(<Harness onProject={(p) => (latest = p)} />)

    // Main is selected by default; add a network and switch it into graphical mode.
    fireEvent.click(screen.getByRole('button', { name: /\+ Add network/ }))
    fireEvent.click(screen.getByRole('button', { name: /switch to graphical ladder/i }))

    // Starts as a single empty contact -- name it, then turn it into an OR branch.
    fireEvent.change(screen.getByPlaceholderText('Tag'), { target: { value: 'Start' } })
    fireEvent.click(screen.getByRole('button', { name: '+ OR branch' }))
    const tagInputs1 = screen.getAllByPlaceholderText('Tag')
    fireEvent.change(tagInputs1[1], { target: { value: 'Motor' } })

    // Add the series NOT Stop contact after the OR group.
    fireEvent.click(screen.getByRole('button', { name: '+ Series contact' }))
    const tagInputs2 = screen.getAllByPlaceholderText('Tag')
    fireEvent.change(tagInputs2[2], { target: { value: 'Stop' } })
    const selects = screen.getAllByRole('combobox')
    fireEvent.change(selects[selects.length - 1], { target: { value: 'NC' } })

    // Drive the coil.
    fireEvent.click(screen.getByRole('button', { name: '+ Add coil' }))
    fireEvent.change(screen.getByPlaceholderText('Target tag'), { target: { value: 'Motor' } })

    const source = compileProject(latest)
    expect(source).toContain('RUNG (Start OR Motor) AND NOT Stop => Motor;')
  })
})
