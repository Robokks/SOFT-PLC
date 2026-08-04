import { afterEach, describe, expect, it, vi } from 'vitest'
import { render, screen, fireEvent, waitFor } from '@testing-library/react'
import { ProgramEditor } from './ProgramEditor'

function mockFetchOnce(ok: boolean, body: unknown) {
  vi.stubGlobal(
    'fetch',
    vi.fn().mockResolvedValue({
      ok,
      status: ok ? 200 : 400,
      json: async () => body,
    }),
  )
}

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('ProgramEditor', () => {
  it('shows the download result and notifies the parent on success', async () => {
    mockFetchOnce(true, { programName: 'Blink', tagCount: 3 })
    const onDownloaded = vi.fn()
    render(<ProgramEditor onDownloaded={onDownloaded} />)

    fireEvent.click(screen.getByRole('button', { name: /download/i }))

    await waitFor(() => expect(onDownloaded).toHaveBeenCalledTimes(1))
    expect(await screen.findByText(/loaded 'Blink' \(3 tags\)/)).toBeInTheDocument()
  })

  it('surfaces a compile error and does not notify the parent', async () => {
    mockFetchOnce(false, { error: 'unexpected token' })
    const onDownloaded = vi.fn()
    render(<ProgramEditor onDownloaded={onDownloaded} />)

    fireEvent.click(screen.getByRole('button', { name: /download/i }))

    expect(await screen.findByText('unexpected token')).toBeInTheDocument()
    expect(onDownloaded).not.toHaveBeenCalled()
  })
})
