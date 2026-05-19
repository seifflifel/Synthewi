export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  props: Partial<HTMLElementTagNameMap[K]> & { className?: string } = {},
  children: Array<Node | string> = [],
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag)
  Object.assign(node, props)
  for (const child of children) {
    node.append(child instanceof Node ? child : document.createTextNode(child))
  }
  return node
}

export function clamp(n: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, n))
}

export function fmtHz(hz: number): string {
  if (!Number.isFinite(hz)) return String(hz)
  if (hz >= 1000) return `${(hz / 1000).toFixed(1)}k`
  return `${Math.round(hz)}`
}

export function fmtMs(ms: number): string {
  if (!Number.isFinite(ms)) return String(ms)
  if (ms >= 1000) return `${(ms / 1000).toFixed(1)}s`
  return `${Math.round(ms)}ms`
}

export function fmtPct(p: number): string {
  if (!Number.isFinite(p)) return String(p)
  return `${Math.round(p)}%`
}

export type Unsubscribe = () => void
