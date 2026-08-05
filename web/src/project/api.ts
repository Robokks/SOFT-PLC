import type { Project } from './types'

// Thin wrappers over PlcServer's project-storage endpoints (see
// docs/architecture.md's "Project storage" section). The backend treats a project as
// an opaque JSON blob -- these functions own turning that blob into/out of a typed
// Project on this side.

export interface ProjectSummary {
  name: string
  updatedAt: string
}

async function readErrorMessage(res: Response, fallback: string): Promise<string> {
  try {
    const body = (await res.json()) as { error?: string }
    return body.error ?? fallback
  } catch {
    return fallback
  }
}

export async function listProjects(): Promise<ProjectSummary[]> {
  const res = await fetch('/api/projects')
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `listing projects failed (${res.status})`))
  }
  return (await res.json()) as ProjectSummary[]
}

export async function getActiveProjectName(): Promise<string | null> {
  const res = await fetch('/api/projects/active')
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `reading active project failed (${res.status})`))
  }
  const body = (await res.json()) as { name: string | null }
  return body.name
}

export async function getProject(name: string): Promise<Project> {
  const res = await fetch(`/api/projects/${encodeURIComponent(name)}`)
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `loading project failed (${res.status})`))
  }
  return (await res.json()) as Project
}

export async function saveProject(project: Project): Promise<void> {
  const res = await fetch(`/api/projects/${encodeURIComponent(project.name)}`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(project),
  })
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `saving project failed (${res.status})`))
  }
}

export async function deleteProject(name: string): Promise<void> {
  const res = await fetch(`/api/projects/${encodeURIComponent(name)}`, { method: 'DELETE' })
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `deleting project failed (${res.status})`))
  }
}

export async function activateProject(name: string): Promise<void> {
  const res = await fetch(`/api/projects/${encodeURIComponent(name)}/activate`, { method: 'POST' })
  if (!res.ok) {
    throw new Error(await readErrorMessage(res, `activating project failed (${res.status})`))
  }
}
