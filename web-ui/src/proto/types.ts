export type WsClientToDevice =
  | { type: 'hello'; v: 1; client: 'synthewi-web-ui'; ts: number }
  | { type: 'set'; v: 1; id: string; path: string; value: number | boolean | string }
  | { type: 'note'; v: 1; note: number; gate: 0 | 1; pressure?: number }
  | { type: 'ping'; v: 1; ts: number }

export type TouchTelemetry = {
  raw?: number
  baseline?: number
  abs_delta?: number
  threshold?: number
  intensity?: number
  is_touching?: boolean
}

export type WsDeviceToClient =
  | { type: 'hello'; v: 1; device: string; caps?: Record<string, boolean>; ts: number }
  | { type: 'telemetry'; v: 1; ts: number; touch?: Record<string, TouchTelemetry> }
  | { type: 'ack'; v: 1; id: string }
  | { type: 'error'; v: 1; id?: string; message: string }
  | { type: 'pong'; v: 1; ts: number }

export function safeJsonParse(input: string): unknown {
  try {
    return JSON.parse(input)
  } catch {
    return null
  }
}
