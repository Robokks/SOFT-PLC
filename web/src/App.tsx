import { useEffect, useState } from 'react'
import { ProjectPicker } from './project/ProjectPicker'
import { ProjectView } from './project/ProjectView'
import { getActiveProjectName, getProject } from './project/api'
import type { Project } from './project/types'

function App() {
  const [project, setProject] = useState<Project | null>(null)
  // Distinguishes "haven't checked yet" from "checked, nothing active" so the
  // picker doesn't flash empty before the auto-open attempt below resolves.
  const [checkedActive, setCheckedActive] = useState(false)

  // Mirrors apps/plc_server's own "already-loaded program automatically starts"
  // behavior at the UI level: open whichever project was last active, instead of
  // always landing on the picker.
  useEffect(() => {
    getActiveProjectName()
      .then((name) => (name ? getProject(name) : null))
      .then(setProject)
      .catch(() => setProject(null))
      .finally(() => setCheckedActive(true))
  }, [])

  if (!checkedActive) {
    return null
  }

  if (!project) {
    return <ProjectPicker onOpen={setProject} />
  }

  return <ProjectView project={project} onChange={setProject} onClose={() => setProject(null)} />
}

export default App
