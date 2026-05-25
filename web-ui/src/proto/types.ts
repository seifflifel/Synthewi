export type WsClientToDevice =
  | { type: 'hello'; v: 1; client: 'synthewi-web-ui'; ts: number }
  | { type: 'set'; v: 1; id: string; path: string; value: number | boolean | string }
  | { type: 'ping'; v: 1; ts: number }

export type TouchTelemetry = {
  raw?: number
  baseline?: number
  abs_delta?: number
  threshold?: number
  intensity?: number
  is_touching?: boolean
}

// All uint16 values from the ESP (0-10000 scale), plus filter_type uint8
export type SynthState = {
  wave_id: number
  filter_type: number
  reverb_amount: number
  reverb_decay: number
  echo_amount: number
  echo_feedback: number
  filter_cutoff: number
  filter_resonance: number
  env_attack: number
  env_release: number
  filter_env_depth: number
  filter_env_decay: number
  lfo_rate: number
  lfo_depth: number
  chorus_amount: number
}

export type WsDeviceToClient =
  | { type: 'hello'; v: 1; device: string; caps?: Record<string, boolean>; ts: number }
  | { type: 'telemetry'; v: 1; ts: number; touch?: Record<string, TouchTelemetry>; synth?: SynthState }
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
