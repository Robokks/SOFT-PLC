import { afterEach, describe, expect, it, vi } from 'vitest'
import { downloadProgram, fetchTags, forceTag } from './api'

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('api', () => {
  it('fetchTags returns the parsed JSON array on success', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({
        ok: true,
        status: 200,
        json: async () => [{ name: 'Counter', type: 'DINT', value: 1 }],
      }),
    )
    await expect(fetchTags()).resolves.toEqual([{ name: 'Counter', type: 'DINT', value: 1 }])
  })

  it('downloadProgram rejects with the backend error message on failure', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({
        ok: false,
        status: 400,
        json: async () => ({ error: 'expected PROGRAM' }),
      }),
    )
    await expect(downloadProgram('garbage')).rejects.toThrow('expected PROGRAM')
  })

  it('downloadProgram falls back to a generic message when the body is not JSON', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue({
        ok: false,
        status: 500,
        json: async () => {
          throw new Error('not json')
        },
      }),
    )
    await expect(downloadProgram('garbage')).rejects.toThrow('download failed (500)')
  })

  it('forceTag posts the JSON-encoded value to the per-tag endpoint', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => ({}) })
    vi.stubGlobal('fetch', fetchMock)

    await forceTag('Speed', 1500)

    expect(fetchMock).toHaveBeenCalledWith(
      '/api/tags/Speed',
      expect.objectContaining({
        method: 'POST',
        body: JSON.stringify({ value: 1500 }),
      }),
    )
  })
})
