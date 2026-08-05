// Tiny immutable-array-update helpers shared by every editor in project/ -- kept
// generic and dependency-free rather than pulling in a structural-editing library for
// what's always just "replace/remove/append one item in one array".

export function replaceAt<T>(arr: T[], index: number, item: T): T[] {
  const copy = arr.slice()
  copy[index] = item
  return copy
}

export function removeAt<T>(arr: T[], index: number): T[] {
  return arr.filter((_, i) => i !== index)
}

export function appendItem<T>(arr: T[], item: T): T[] {
  return [...arr, item]
}
