import { afterEach, describe, expect, it, vi } from 'vitest'
import { fireEvent, render, screen, waitFor } from '@testing-library/react'
import { ProjectPicker } from './ProjectPicker'

function mockFetchSequence(responses: { ok: boolean; body: unknown }[]) {
  const fetchMock = vi.fn()
  for (const r of responses) {
    fetchMock.mockResolvedValueOnce({ ok: r.ok, status: r.ok ? 200 : 400, json: async () => r.body })
  }
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('ProjectPicker', () => {
  it('shows an empty state when there are no projects', async () => {
    mockFetchSequence([
      { ok: true, body: [] }, // GET /api/projects
      { ok: true, body: { name: null } }, // GET /api/projects/active
    ])
    render(<ProjectPicker onOpen={vi.fn()} />)

    expect(await screen.findByText(/no projects yet/i)).toBeInTheDocument()
  })

  it('lists projects and opens one on click', async () => {
    mockFetchSequence([
      { ok: true, body: [{ name: 'Line1', updatedAt: '2026-01-01T00:00:00Z' }] },
      { ok: true, body: { name: 'Line1' } },
      { ok: true, body: { name: 'Line1', ioLinking: [], driveConfig: [], dataBlocks: [], blocks: [] } },
    ])
    const onOpen = vi.fn()
    render(<ProjectPicker onOpen={onOpen} />)

    const openButton = await screen.findByRole('button', { name: 'Line1' })
    expect(screen.getByText('active')).toBeInTheDocument()
    fireEvent.click(openButton)

    await waitFor(() =>
      expect(onOpen).toHaveBeenCalledWith(
        expect.objectContaining({ name: 'Line1' }),
      ),
    )
  })

  it('creates a new project and opens it', async () => {
    mockFetchSequence([
      { ok: true, body: [] },
      { ok: true, body: { name: null } },
      { ok: true, body: { ok: true } }, // PUT /api/projects/NewLine
    ])
    const onOpen = vi.fn()
    render(<ProjectPicker onOpen={onOpen} />)

    await screen.findByText(/no projects yet/i)
    fireEvent.change(screen.getByPlaceholderText(/new project name/i), {
      target: { value: 'NewLine' },
    })
    fireEvent.click(screen.getByRole('button', { name: /create project/i }))

    await waitFor(() =>
      expect(onOpen).toHaveBeenCalledWith(
        expect.objectContaining({ name: 'NewLine', blocks: [expect.objectContaining({ kind: 'Main' })] }),
      ),
    )
  })
})
