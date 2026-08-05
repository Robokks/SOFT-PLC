import { useCallback, useEffect, useRef, useState } from 'react'
import { downloadProgram, fetchStatus, fetchTags, subscribeTagStream } from '../api'
import { StatusBar } from '../components/StatusBar'
import { TagTable } from '../components/TagTable'
import type { StatusInfo, TagInfo } from '../types'
import { activateProject, saveProject } from './api'
import { compileProject, ProjectCompileError } from './compile'
import { BlockTree } from './BlockTree'
import { DataBlocksSection } from './DataBlocksSection'
import { IoLinkingSection } from './IoLinkingSection'
import { DriveConfigSection } from './DriveConfigSection'
import type { Project } from './types'

type Tab = 'programming' | 'datablocks' | 'io' | 'drive'

const kStatusPollMs = 1000
const kTagPollFallbackMs = 1000

// Everything "under" an open project (see the original request this implements):
// IO linking, drive configuration, DB creation, and programming, plus a live
// monitor of whatever PlcServer is actually running -- which may or may not be this
// project, if a different one was compiled more recently.
export function ProjectView({
  project,
  onChange,
  onClose,
}: {
  project: Project
  onChange: (project: Project) => void
  onClose: () => void
}) {
  const [tab, setTab] = useState<Tab>('programming')
  const [status, setStatus] = useState<StatusInfo | null>(null)
  const [tags, setTags] = useState<TagInfo[]>([])
  const streamLiveRef = useRef(false)
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  const refreshTagsOnce = useCallback(() => {
    fetchTags().then(setTags).catch(() => {
      // A download briefly restarts the engine; a poll landing in that window just
      // fails silently and tries again next tick.
    })
  }, [])

  useEffect(() => {
    const poll = () => {
      fetchStatus().then(setStatus).catch(() => setStatus(null))
    }
    poll()
    const id = setInterval(poll, kStatusPollMs)
    return () => clearInterval(id)
  }, [])

  useEffect(() => {
    const unsubscribe = subscribeTagStream(
      (nextTags) => {
        streamLiveRef.current = true
        setTags(nextTags)
      },
      () => {
        streamLiveRef.current = false
      },
    )
    const fallback = setInterval(() => {
      if (!streamLiveRef.current) {
        refreshTagsOnce()
      }
    }, kTagPollFallbackMs)
    return () => {
      unsubscribe()
      clearInterval(fallback)
    }
  }, [refreshTagsOnce])

  async function handleSave() {
    setBusy(true)
    setError(null)
    try {
      await saveProject(project)
      setMessage('Saved.')
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setBusy(false)
    }
  }

  async function handleCompileAndDownload() {
    setBusy(true)
    setError(null)
    setMessage(null)
    try {
      const compiledSource = compileProject(project)
      const withSource = { ...project, compiledSource }
      const result = await downloadProgram(compiledSource)
      await saveProject(withSource)
      await activateProject(project.name)
      onChange(withSource)
      setMessage(`Downloaded '${result.programName}' (${result.tagCount} tags).`)
      refreshTagsOnce()
    } catch (e) {
      if (e instanceof ProjectCompileError) {
        setError(e.message)
      } else {
        setError(e instanceof Error ? e.message : String(e))
      }
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="project-view">
      <header className="project-view__header">
        <button className="project-view__close" onClick={onClose}>
          &larr; Projects
        </button>
        <h1>{project.name}</h1>
        <nav className="project-view__tabs">
          <button className={tab === 'programming' ? 'active' : ''} onClick={() => setTab('programming')}>
            Programming
          </button>
          <button className={tab === 'datablocks' ? 'active' : ''} onClick={() => setTab('datablocks')}>
            Data Blocks
          </button>
          <button className={tab === 'io' ? 'active' : ''} onClick={() => setTab('io')}>
            IO Linking
          </button>
          <button className={tab === 'drive' ? 'active' : ''} onClick={() => setTab('drive')}>
            Drive Configuration
          </button>
        </nav>
        <div className="project-view__actions">
          <button disabled={busy} onClick={handleSave}>
            Save
          </button>
          <button disabled={busy} onClick={handleCompileAndDownload}>
            Compile &amp; Download
          </button>
        </div>
      </header>

      {message && <p className="project-view__message">{message}</p>}
      {error && <pre className="project-view__error">{error}</pre>}

      <div className="project-view__body">
        {tab === 'programming' && <BlockTree project={project} onChange={onChange} />}
        {tab === 'datablocks' && <DataBlocksSection project={project} onChange={onChange} />}
        {tab === 'io' && <IoLinkingSection project={project} onChange={onChange} />}
        {tab === 'drive' && <DriveConfigSection project={project} onChange={onChange} />}
      </div>

      <section>
        <h2>Live monitor</h2>
        <StatusBar status={status} />
        <TagTable tags={tags} />
      </section>
    </div>
  )
}
