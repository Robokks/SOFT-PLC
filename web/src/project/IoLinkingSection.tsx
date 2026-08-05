import { VarDeclList } from './VarDeclList'
import type { Project } from './types'

// IO linking: tags with a direct IEC address (%I/%Q/%M), declared into Main's VAR
// section on compile (see compile.ts's emitMainVarSections) -- the same VarDeclList
// used everywhere else, just with the address column turned on.
export function IoLinkingSection({
  project,
  onChange,
}: {
  project: Project
  onChange: (project: Project) => void
}) {
  return (
    <div className="io-linking-section">
      <p className="io-linking-section__hint">
        Tags with a direct address (%I/%Q/%M), available by name from any network.
      </p>
      <VarDeclList
        label="IO tags"
        decls={project.ioLinking}
        showAddress
        onChange={(ioLinking) => onChange({ ...project, ioLinking })}
      />
    </div>
  )
}
