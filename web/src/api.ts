import type { StatusInfo, TagInfo, TagValue } from './types'

// Thin fetch wrappers over PlcServer's HTTP API (see docs/architecture.md's
// "Programming/monitoring HTTP server" section). All paths are same-origin: in dev,
// vite.config.ts proxies /api to the backend; in production PlcServer serves this
// app's own build output, so no base URL is needed either way.

async function readErrorMessage(res: Response, fallback: string): Promise<string> {
  try {
    const body = (await res.json()) as { error?: string }
    return body.error ?? fallback
  } catch {
    return fallback
  }
}

export async function fetchStatus(): Promise<StatusInfo> {
  const res = await fetch('/api/status')
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `status request failed (${res.status})`))
  }
  return (await res.json()) as StatusInfo
}

export async function fetchTags(): Promise<TagInfo[]> {
  const res = await fetch('/api/tags')
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `tags request failed (${res.status})`))
  }
  return (await res.json()) as TagInfo[]
}

export async function downloadProgram(
  source: string,
): Promise<{ programName: string; tagCount: number }> {
  const res = await fetch('/api/program', {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: source,
  })
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `download failed (${res.status})`))
  }
  return (await res.json()) as { programName: string; tagCount: number }
}

export async function forceTag(name: string, value: TagValue): Promise<void> {
  const res = await fetch(`/api/tags/${encodeURIComponent(name)}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ value }),
  })
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `force failed (${res.status})`))
  }
}

// Subscribes to the live tag stream (SSE). Returns an unsubscribe function. onError
// fires on any stream error (e.g. the backend restarting during a download) so the
// caller can fall back to polling -- see useTagStream in App.tsx.
export function subscribeTagStream(
  onTags: (tags: TagInfo[]) => void,
  onError: () => void,
): () => void {
  const source = new EventSource('/api/tags/stream')
  source.onmessage = (event: MessageEvent<string>) => {
    try {
      onTags(JSON.parse(event.data) as TagInfo[])
    } catch {
      // Malformed frame -- ignore it and wait for the next one rather than tearing
      // down an otherwise-healthy stream.
    }
  }
  source.onerror = onError
  return () => source.close()
}
