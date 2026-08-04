import type { TagInfo } from '../types'
import { ForceControl } from './ForceControl'

export function TagTable({ tags }: { tags: TagInfo[] }) {
  if (tags.length === 0) {
    return <p className="tag-table__empty">No tags -- download a program to see its variables here.</p>
  }

  return (
    <table className="tag-table">
      <thead>
        <tr>
          <th>Name</th>
          <th>Type</th>
          <th>Address</th>
          <th>Value</th>
          <th>Force</th>
        </tr>
      </thead>
      <tbody>
        {tags.map((tag) => (
          <tr key={tag.name}>
            <td>{tag.name}</td>
            <td>{tag.type}</td>
            <td>{tag.address ?? ''}</td>
            <td>{String(tag.value)}</td>
            <td>
              <ForceControl tag={tag} />
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}
