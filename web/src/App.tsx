import { useCallback, useEffect, useRef, useState } from 'react'
import type { StatusInfo, TagInfo } from './types'
import { fetchStatus, fetchTags, subscribeTagStream } from './api'
import { StatusBar } from './components/StatusBar'
import { TagTable } from './components/TagTable'
import { ProgramEditor } from './components/ProgramEditor'

const kStatusPollMs = 1000
const kTagPollFallbackMs = 1000

function App() {
  const [status, setStatus] = useState<StatusInfo | null>(null)
  const [tags, setTags] = useState<TagInfo[]>([])
  // True once the SSE stream has delivered at least one frame -- while it's live we
  // skip the tag poll fallback below entirely, rather than running both at once.
  const streamLiveRef = useRef(false)

  const refreshTagsOnce = useCallback(() => {
    fetchTags()
      .then(setTags)
      .catch(() => {
        // A download briefly restarts the engine; a poll landing in that window just
        // fails silently and tries again next tick rather than surfacing a flash of
        // error UI for an expected, momentary condition.
      })
  }, [])

  useEffect(() => {
    const poll = () => {
      fetchStatus()
        .then(setStatus)
        .catch(() => setStatus(null))
    }
    poll()
    const id = setInterval(poll, kStatusPollMs)
    return () => clearInterval(id)
  }, [])

  // Live tag stream, with a plain poll as a fallback for whenever SSE isn't flowing
  // (initial connection, or the momentary gap while a download restarts the engine).
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

  return (
    <div className="app">
      <h1>SOFT-PLC</h1>
      <StatusBar status={status} />
      <section>
        <h2>Program</h2>
        <ProgramEditor onDownloaded={refreshTagsOnce} />
      </section>
      <section>
        <h2>Live tags</h2>
        <TagTable tags={tags} />
      </section>
    </div>
  )
}

export default App
