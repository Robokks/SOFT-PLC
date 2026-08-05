import { afterEach, describe, expect, it, vi } from 'vitest'
import { fireEvent, render, screen, waitFor } from '@testing-library/react'
import { ProjectView } from './ProjectView'
import { emptyProject } from './types'

function jsonResponse(body: unknown, ok = true) {
  return { ok, status: ok ? 200 : 400, json: async () => body, body: '' }
}

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('ProjectView', () => {
  it('compiles, downloads, saves, and activates the project on Compile & Download', async () => {
    const fetchMock = vi.fn((url: string, init?: RequestInit) => {
      if (url === '/api/status') {
        return Promise.resolve(jsonResponse({ running: false, programName: '', tagCount: 0 }))
      }
      if (url === '/api/tags') {
        return Promise.resolve(jsonResponse([]))
      }
      if (url === '/api/program' && init?.method === 'POST') {
        expect(init.body).toContain('PROGRAM Main')
        return Promise.resolve(jsonResponse({ programName: 'Main', tagCount: 0 }))
      }
      if (url === '/api/projects/Line1' && init?.method === 'PUT') {
        const saved = JSON.parse(init.body as string)
        expect(saved.compiledSource).toContain('PROGRAM Main')
        return Promise.resolve(jsonResponse({ ok: true }))
      }
      if (url === '/api/projects/Line1/activate' && init?.method === 'POST') {
        return Promise.resolve(jsonResponse({ ok: true }))
      }
      throw new Error(`unexpected fetch: ${url}`)
    })
    vi.stubGlobal('fetch', fetchMock)
    vi.stubGlobal(
      'EventSource',
      class {
        close() {}
      },
    )

    const project = emptyProject('Line1')
    render(<ProjectView project={project} onChange={vi.fn()} onClose={vi.fn()} />)

    fireEvent.click(screen.getByRole('button', { name: /compile & download/i }))

    await waitFor(() => expect(screen.getByText(/downloaded 'main'/i)).toBeInTheDocument())
    expect(fetchMock).toHaveBeenCalledWith('/api/program', expect.objectContaining({ method: 'POST' }))
    expect(fetchMock).toHaveBeenCalledWith(
      '/api/projects/Line1',
      expect.objectContaining({ method: 'PUT' }),
    )
    expect(fetchMock).toHaveBeenCalledWith(
      '/api/projects/Line1/activate',
      expect.objectContaining({ method: 'POST' }),
    )
  })

  it('shows a compile error and does not call the backend when the project has no Main block', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(jsonResponse({ running: false, programName: '', tagCount: 0 })))
    vi.stubGlobal(
      'EventSource',
      class {
        close() {}
      },
    )

    const project = { ...emptyProject('Line1'), blocks: [] }
    render(<ProjectView project={project} onChange={vi.fn()} onClose={vi.fn()} />)

    fireEvent.click(screen.getByRole('button', { name: /compile & download/i }))

    expect(await screen.findByText(/no Main block/i)).toBeInTheDocument()
  })
})
