import { useState } from 'react'
import type { TagInfo, TagValue } from '../types'
import { forceTag } from '../api'

// One tag's "force" control: BOOL gets a toggle button, everything else (numeric
// types and STRING) gets a text input committed on Enter/blur -- TIME is written as
// its raw millisecond count, matching how PlcServer::writeTag() special-cases a
// numeric write into a TIME-declared tag (see docs/architecture.md).
export function ForceControl({ tag }: { tag: TagInfo }) {
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [draft, setDraft] = useState(String(tag.value))

  async function commit(value: TagValue) {
    setPending(true)
    setError(null)
    try {
      await forceTag(tag.name, value)
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setPending(false)
    }
  }

  if (tag.type === 'BOOL') {
    return (
      <div className="force-control">
        <button disabled={pending} onClick={() => commit(!tag.value)}>
          {tag.value ? 'set FALSE' : 'set TRUE'}
        </button>
        {error && <span className="force-control__error">{error}</span>}
      </div>
    )
  }

  const isNumeric = tag.type !== 'STRING'

  return (
    <div className="force-control">
      <input
        value={draft}
        disabled={pending}
        onChange={(e) => setDraft(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === 'Enter') {
            commit(isNumeric ? Number(draft) : draft)
          }
        }}
        onBlur={() => commit(isNumeric ? Number(draft) : draft)}
      />
      {error && <span className="force-control__error">{error}</span>}
    </div>
  )
}
