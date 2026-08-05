import { useEffect, useState } from 'react'
import { deleteProject, getActiveProjectName, getProject, listProjects, saveProject } from './api'
import type { ProjectSummary } from './api'
import { emptyProject } from './types'
import type { Project } from './types'

// The landing screen: list existing projects (create/open/delete), or create a new
// one -- "provision to open or create a project" from the original request. Shown
// whenever no project is currently open; ProjectView (opened via onOpen) hosts
// everything "under" a project (IO linking, drive config, DBs, programming).
export function ProjectPicker({ onOpen }: { onOpen: (project: Project) => void }) {
  const [projects, setProjects] = useState<ProjectSummary[]>([])
  const [activeName, setActiveName] = useState<string | null>(null)
  const [newName, setNewName] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)

  function refresh() {
    Promise.all([listProjects(), getActiveProjectName()])
      .then(([list, active]) => {
        setProjects(list)
        setActiveName(active)
      })
      .catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)))
  }

  useEffect(refresh, [])

  async function handleCreate() {
    const name = newName.trim()
    if (!name) {
      return
    }
    setBusy(true)
    setError(null)
    try {
      const project = emptyProject(name)
      await saveProject(project)
      onOpen(project)
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setBusy(false)
    }
  }

  async function handleOpen(name: string) {
    setBusy(true)
    setError(null)
    try {
      onOpen(await getProject(name))
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
      setBusy(false)
    }
  }

  async function handleDelete(name: string) {
    setBusy(true)
    setError(null)
    try {
      await deleteProject(name)
      refresh()
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="project-picker">
      <h1>SOFT-PLC</h1>
      <h2>Projects</h2>
      {projects.length === 0 ? (
        <p className="project-picker__empty">No projects yet -- create one below.</p>
      ) : (
        <ul className="project-picker__list">
          {projects.map((p) => (
            <li key={p.name}>
              <button disabled={busy} onClick={() => handleOpen(p.name)}>
                {p.name}
              </button>
              {p.name === activeName && <span className="project-picker__active">active</span>}
              <span className="project-picker__updated">{p.updatedAt}</span>
              <button
                className="project-picker__delete"
                disabled={busy}
                onClick={() => handleDelete(p.name)}
              >
                Delete
              </button>
            </li>
          ))}
        </ul>
      )}

      <div className="project-picker__create">
        <input
          placeholder="New project name"
          value={newName}
          disabled={busy}
          onChange={(e) => setNewName(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter') {
              handleCreate()
            }
          }}
        />
        <button disabled={busy || newName.trim() === ''} onClick={handleCreate}>
          Create project
        </button>
      </div>
      {error && <pre className="project-picker__error">{error}</pre>}
    </div>
  )
}
