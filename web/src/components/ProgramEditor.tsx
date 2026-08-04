import { useState } from 'react'
import { downloadProgram } from '../api'

const kPlaceholder = `PROGRAM Example
VAR
    Counter : DINT := 0;
END_VAR
Counter := Counter + 1;
END_PROGRAM
`

// A raw ST-source textarea and a "Download" button -- v1's only program-entry path
// (see docs/roadmap.md: the graphical Ladder editor and IL/STL front end are still
// open work). A failed download leaves whatever was previously running untouched
// (PlcServer::download()'s guarantee), so the error surfaced here is purely
// informational, not a sign anything on the target broke.
export function ProgramEditor({ onDownloaded }: { onDownloaded: () => void }) {
  const [source, setSource] = useState(kPlaceholder)
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [lastResult, setLastResult] = useState<string | null>(null)

  async function handleDownload() {
    setPending(true)
    setError(null)
    try {
      const result = await downloadProgram(source)
      setLastResult(`loaded '${result.programName}' (${result.tagCount} tags)`)
      onDownloaded()
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setPending(false)
    }
  }

  return (
    <div className="program-editor">
      <textarea
        value={source}
        onChange={(e) => setSource(e.target.value)}
        spellCheck={false}
        rows={14}
      />
      <div className="program-editor__actions">
        <button disabled={pending} onClick={handleDownload}>
          {pending ? 'Downloading…' : 'Download'}
        </button>
        {lastResult && <span className="program-editor__result">{lastResult}</span>}
      </div>
      {error && <pre className="program-editor__error">{error}</pre>}
    </div>
  )
}
