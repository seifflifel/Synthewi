import dgram from 'node:dgram'
import { WebSocketServer, WebSocket } from 'ws'

type TouchChannel = {
  raw: number
  baseline: number
  abs_delta: number
  threshold: number
  intensity: number
  is_touching: boolean
}

type TouchTelemetry = Record<string, TouchChannel>

type SynthState = {
  wave_id: number
  reverb_amount: number
  reverb_decay: number
  echo_amount: number
  echo_feedback: number
  filter_cutoff: number
  filter_resonance: number
  env_attack: number
  env_release: number
}

type DeviceState = {
  address: string
  port: number
  lastSeenMs: number
  mac?: string
}

type WsClientToDevice =
  | { type: 'hello'; v: 1; client: 'synthewi-web-ui'; ts: number }
  | { type: 'set'; v: 1; id: string; path: string; value: number | boolean | string }
  | { type: 'ping'; v: 1; ts: number }

type WsDeviceToClient =
  | { type: 'hello'; v: 1; device: string; caps?: Record<string, boolean>; ts: number }
  | { type: 'telemetry'; v: 1; ts: number; touch?: TouchTelemetry; synth?: SynthState }
  | { type: 'ack'; v: 1; id: string }
  | { type: 'error'; v: 1; id?: string; message: string }
  | { type: 'pong'; v: 1; ts: number }

const WS_PORT = Number.parseInt(process.env.BRIDGE_WS_PORT ?? '8787', 10)
const WS_PATH = process.env.BRIDGE_WS_PATH ?? '/ws'
const TELEMETRY_PORT = Number.parseInt(process.env.BRIDGE_UDP_PORT ?? '4210', 10)
const CMD_PORT = Number.parseInt(process.env.BRIDGE_CMD_PORT ?? '4211', 10)

const MAGIC_TELEM = Buffer.from('SYNT')
const MAGIC_CMD = Buffer.from('SYNC')
const PROTO_VERSION = 2
const CHANNELS = 4

const CMD_SET_THRESHOLD   = 1
const CMD_SET_SYNTH_PARAM = 2

// Maps UI path → { paramId (matches SYNTH_PARAM_* in touch_telemetry.h), scale }
// scale=1: value is already a direct integer (wave_id 0-4)
// scale=10000: value is a 0-1 float from displayToNorm — multiply to get ESP 0-10000
const PATH_TO_CMD: Record<string, { paramId: number; scale: number }> = {
  'synth.wave':          { paramId: 0, scale: 1     },
  'fx.reverb.amount':    { paramId: 1, scale: 10000 },
  'fx.reverb.decay':     { paramId: 2, scale: 10000 },
  'fx.echo.amount':      { paramId: 3, scale: 10000 },
  'fx.echo.feedback':    { paramId: 4, scale: 10000 },
  'fx.filter.cutoff':    { paramId: 5, scale: 10000 },
  'fx.filter.resonance': { paramId: 6, scale: 10000 },
  'env.attack':          { paramId: 7, scale: 10000 },
  'env.release':         { paramId: 8, scale: 10000 },
}

let lastDevice: DeviceState | null = null
let lastTouch: TouchTelemetry | null = null
let lastSynth: SynthState | null = null

function safeJsonParse(input: string): unknown {
  try {
    return JSON.parse(input)
  } catch {
    return null
  }
}

function macToString(buf: Buffer): string {
  const parts: string[] = []
  for (const b of buf) parts.push(b.toString(16).padStart(2, '0'))
  return parts.join(':')
}

function parseTelemetry(buf: Buffer): { seq: number; mac?: string; touch: TouchTelemetry; synth?: SynthState } | null {
  const minSize = 16 + CHANNELS * 8
  if (buf.length < minSize) return null
  if (!buf.subarray(0, 4).equals(MAGIC_TELEM)) return null
  const version = buf.readUInt8(4)
  if (version !== PROTO_VERSION) return null

  const seq = buf.readUInt16LE(6)
  const macBuf = buf.subarray(8, 14)
  const channelCount = buf.readUInt8(14)
  if (channelCount < CHANNELS) return null

  const touch: TouchTelemetry = {}
  for (let i = 0; i < CHANNELS; i++) {
    const base = 16 + i * 8
    const raw = buf.readUInt16LE(base)
    const baseline = buf.readUInt16LE(base + 2)
    const threshold = buf.readUInt16LE(base + 4)
    const is_touching = buf.readUInt8(base + 6) !== 0
    const intensity = buf.readUInt8(base + 7)
    const abs_delta = Math.abs(raw - baseline)
    touch[`ch${i}`] = { raw, baseline, abs_delta, threshold, intensity, is_touching }
  }

  // Parse v2 synth state (18 bytes after channel data)
  let synth: SynthState | undefined
  const synthBase = 16 + CHANNELS * 8
  if (buf.length >= synthBase + 18) {
    synth = {
      wave_id:          buf.readUInt8(synthBase + 0),
      reverb_amount:    buf.readUInt16LE(synthBase + 2),
      reverb_decay:     buf.readUInt16LE(synthBase + 4),
      echo_amount:      buf.readUInt16LE(synthBase + 6),
      echo_feedback:    buf.readUInt16LE(synthBase + 8),
      filter_cutoff:    buf.readUInt16LE(synthBase + 10),
      filter_resonance: buf.readUInt16LE(synthBase + 12),
      env_attack:       buf.readUInt16LE(synthBase + 14),
      env_release:      buf.readUInt16LE(synthBase + 16),
    }
  }

  return { seq, mac: macToString(macBuf), touch, synth }
}

function buildThresholdCmd(channel: number, threshold: number): Buffer {
  const buf = Buffer.alloc(10)
  MAGIC_CMD.copy(buf, 0)
  buf.writeUInt8(PROTO_VERSION, 4)
  buf.writeUInt8(CMD_SET_THRESHOLD, 5)
  buf.writeUInt8(channel, 6)
  buf.writeUInt8(0, 7)
  buf.writeUInt16LE(threshold, 8)
  return buf
}

function buildSynthParamCmd(paramId: number, value: number): Buffer {
  const buf = Buffer.alloc(10)
  MAGIC_CMD.copy(buf, 0)
  buf.writeUInt8(PROTO_VERSION, 4)
  buf.writeUInt8(CMD_SET_SYNTH_PARAM, 5)
  buf.writeUInt8(paramId, 6)   // channel field = param_id
  buf.writeUInt8(0, 7)
  buf.writeUInt16LE(value, 8)  // threshold field = value 0-10000
  return buf
}

const udpRx = dgram.createSocket('udp4')
const udpTx = dgram.createSocket('udp4')

udpRx.on('error', (err) => {
  console.error('[udp] error', err)
})

udpRx.on('message', (msg, rinfo) => {
  const parsed = parseTelemetry(msg)
  if (!parsed) return
  lastDevice = {
    address: rinfo.address,
    port: CMD_PORT,
    lastSeenMs: Date.now(),
    mac: parsed.mac,
  }
  lastTouch = parsed.touch
  if (parsed.synth) lastSynth = parsed.synth
  broadcastTelemetry(parsed.touch, parsed.synth)
})

udpRx.bind(TELEMETRY_PORT, () => {
  udpRx.setBroadcast(true)
  console.log(`[udp] listening on 0.0.0.0:${TELEMETRY_PORT}`)
})

const wss = new WebSocketServer({ port: WS_PORT, path: WS_PATH })

function sendJson(ws: WebSocket, msg: WsDeviceToClient) {
  ws.send(JSON.stringify(msg))
}

function broadcastTelemetry(touch: TouchTelemetry, synth?: SynthState) {
  const payload: WsDeviceToClient = { type: 'telemetry', v: 1, ts: Date.now(), touch, synth }
  const data = JSON.stringify(payload)
  for (const client of wss.clients) {
    if (client.readyState === WebSocket.OPEN) {
      client.send(data)
    }
  }
}

wss.on('connection', (ws) => {
  if (lastTouch) {
    sendJson(ws, { type: 'telemetry', v: 1, ts: Date.now(), touch: lastTouch, synth: lastSynth ?? undefined })
  }

  ws.on('message', (data) => {
    const raw = typeof data === 'string' ? data : data.toString()
    const parsed = safeJsonParse(raw)
    if (!parsed || typeof parsed !== 'object') {
      sendJson(ws, { type: 'error', v: 1, message: 'invalid json' })
      return
    }

    const msg = parsed as WsClientToDevice
    if (msg.type === 'hello') {
      sendJson(ws, {
        type: 'hello',
        v: 1,
        device: 'Synthewi-Bridge',
        caps: { touch: true, synth: true },
        ts: Date.now(),
      })
      return
    }

    if (msg.type === 'ping') {
      sendJson(ws, { type: 'pong', v: 1, ts: Date.now() })
      return
    }

    if (msg.type === 'set') {
      if (typeof msg.value !== 'number' || !Number.isFinite(msg.value)) {
        sendJson(ws, { type: 'error', v: 1, id: msg.id, message: 'invalid value' })
        return
      }
      if (!lastDevice) {
        sendJson(ws, { type: 'error', v: 1, id: msg.id, message: 'no device telemetry yet' })
        return
      }

      // touch.ch[0-3].threshold
      const threshMatch = /^touch\.ch([0-3])\.threshold$/.exec(msg.path)
      if (threshMatch) {
        const channel = Number.parseInt(threshMatch[1], 10)
        const threshold = Math.max(1, Math.min(65535, Math.round(msg.value)))
        const packet = buildThresholdCmd(channel, threshold)
        udpTx.send(packet, lastDevice.port, lastDevice.address, (err) => {
          if (err) { sendJson(ws, { type: 'error', v: 1, id: msg.id, message: 'udp send failed' }); return }
          sendJson(ws, { type: 'ack', v: 1, id: msg.id })
        })
        return
      }

      // synth/fx/env params
      const synthCmd = PATH_TO_CMD[msg.path]
      if (synthCmd) {
        const value = Math.max(0, Math.min(10000, Math.round((msg.value as number) * synthCmd.scale)))
        const packet = buildSynthParamCmd(synthCmd.paramId, value)
        udpTx.send(packet, lastDevice.port, lastDevice.address, (err) => {
          if (err) { sendJson(ws, { type: 'error', v: 1, id: msg.id, message: 'udp send failed' }); return }
          sendJson(ws, { type: 'ack', v: 1, id: msg.id })
        })
        return
      }

      sendJson(ws, { type: 'error', v: 1, id: msg.id, message: `unsupported path: ${msg.path}` })
    }
  })
})

wss.on('listening', () => {
  console.log(`[ws] listening on ws://0.0.0.0:${WS_PORT}${WS_PATH}`)
})
