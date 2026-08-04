import { describe, expect, it } from 'vitest'
import { render, screen } from '@testing-library/react'
import { TagTable } from './TagTable'
import type { TagInfo } from '../types'

describe('TagTable', () => {
  it('shows a placeholder when there are no tags', () => {
    render(<TagTable tags={[]} />)
    expect(screen.getByText(/download a program/i)).toBeInTheDocument()
  })

  it('renders one row per tag with name/type/address/value', () => {
    const tags: TagInfo[] = [
      { name: 'LedState', type: 'BOOL', value: true, address: '%Q0.0' },
      { name: 'Counter', type: 'DINT', value: 42 },
    ]
    render(<TagTable tags={tags} />)

    expect(screen.getByText('LedState')).toBeInTheDocument()
    expect(screen.getByText('%Q0.0')).toBeInTheDocument()
    expect(screen.getByText('true')).toBeInTheDocument()

    expect(screen.getByText('Counter')).toBeInTheDocument()
    expect(screen.getByText('DINT')).toBeInTheDocument()
    expect(screen.getByText('42')).toBeInTheDocument()
  })
})
