import { el, fmtHz, fmtMs, fmtPct, clamp } from '../lib/dom'
import { WsClient, type ConnectionState } from '../proto/wsClient'
import type { TouchTelemetry, WsDeviceToClient } from '../proto/types'

const NOTE_NAMES = ['C4', 'D4', 'E4', 'F4', 'G4', 'A4', 'B4']
const BASE_FREQS = [261.63, 293.66, 329.63, 349.23, 392.0, 440.0, 493.88]
const KEY_MAP: Record<string, number> = { a: 0, s: 1, d: 2, f: 3, g: 4, h: 5, j: 6 }
const TOUCH_THR_MIN = 1
const TOUCH_THR_MAX = 20000

type TouchState = Required<TouchTelemetry> & {
  rawHist: number[]
  baselineHist: number[]
}

type AppState = {
  wsUrl: string
  conn: ConnectionState
  connDetail: string
  lastLine: string
  touch: TouchState[]
}

function defaultWsUrl(): string {
  // PC bridge default
  return 'ws://localhost:8787/ws'
}

function createTouchState(): TouchState {
  return {
    raw: 0,
    baseline: 0,
    abs_delta: 0,
    threshold: 100,
    intensity: 0,
    is_touching: false,
    rawHist: [],
    baselineHist: [],
  }
}

function pushHist(arr: number[], v: number, max = 160) {
  arr.push(v)
  if (arr.length > max) arr.splice(0, arr.length - max)
}

function pillClass(state: ConnectionState): string {
  if (state === 'connected') return 'pill live'
  if (state === 'error') return 'pill err'
  return 'pill demo'
}

function pillText(state: ConnectionState): string {
  if (state === 'connected') return 'live'
  if (state === 'connecting') return 'connecting'
  if (state === 'error') return 'error'
  return 'demo'
}

function nowId(): string {
  return `${Date.now()}-${Math.random().toString(16).slice(2)}`
}

function createParamSender(ws: WsClient) {
  // Slider spam is the biggest enemy for real-time audio; coalesce updates.
  const pending = new Map<string, { value: number | boolean | string; lastSentMs: number }>()
  const minIntervalMs = 33 // ~30Hz

  function tick() {
    const t = Date.now()
    for (const [path, p] of pending.entries()) {
      if (t - p.lastSentMs < minIntervalMs) continue
      ws.send({ type: 'set', v: 1, id: nowId(), path, value: p.value })
      p.lastSentMs = t
      // Keep last value pending (coalescing) until changed; remove to reduce map size.
      pending.delete(path)
    }
    requestAnimationFrame(tick)
  }
  requestAnimationFrame(tick)

  return {
    set(path: string, value: number | boolean | string) {
      const cur = pending.get(path)
      if (cur) cur.value = value
      else pending.set(path, { value, lastSentMs: 0 })
    },
    flush(path: string, value: number | boolean | string) {
      pending.delete(path)
      ws.send({ type: 'set', v: 1, id: nowId(), path, value })
    },
  }
}

function drawSpark(canvas: HTMLCanvasElement, raw: number[], baseline: number[]) {
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  const dpr = window.devicePixelRatio || 1
  const w = canvas.clientWidth
  const h = canvas.clientHeight
  const W = Math.max(1, Math.floor(w * dpr))
  const H = Math.max(1, Math.floor(h * dpr))
  if (canvas.width !== W || canvas.height !== H) {
    canvas.width = W
    canvas.height = H
  }

  ctx.clearRect(0, 0, W, H)

  // Determine scale from recent data (robust-ish)
  const all = raw.concat(baseline)
  let min = Infinity
  let max = -Infinity
  for (const v of all) {
    if (!Number.isFinite(v)) continue
    if (v < min) min = v
    if (v > max) max = v
  }
  if (!Number.isFinite(min) || !Number.isFinite(max) || min === max) {
    min = 0
    max = 1
  }
  const pad = (max - min) * 0.08
  min -= pad
  max += pad

  const toY = (v: number) => {
    const t = (v - min) / (max - min)
    return H - clamp(t, 0, 1) * (H - 4) - 2
  }

  // midline
  ctx.strokeStyle = 'rgba(127,127,127,.18)'
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.moveTo(0, H / 2)
  ctx.lineTo(W, H / 2)
  ctx.stroke()

  const n = Math.max(raw.length, baseline.length)
  const dx = n > 1 ? W / (n - 1) : W

  // baseline (thin)
  ctx.strokeStyle = 'rgba(55,138,221,.55)'
  ctx.lineWidth = 1
  ctx.beginPath()
  for (let i = 0; i < baseline.length; i++) {
    const x = i * dx
    const y = toY(baseline[i])
    if (i === 0) ctx.moveTo(x, y)
    else ctx.lineTo(x, y)
  }
  ctx.stroke()

  // raw (thicker)
  ctx.strokeStyle = 'rgba(175,169,236,.95)'
  ctx.lineWidth = 2
  ctx.beginPath()
  for (let i = 0; i < raw.length; i++) {
    const x = i * dx
    const y = toY(raw[i])
    if (i === 0) ctx.moveTo(x, y)
    else ctx.lineTo(x, y)
  }
  ctx.stroke()
}

export function mountApp(root: HTMLDivElement) {
  const state: AppState = {
    wsUrl: defaultWsUrl(),
    conn: 'disconnected',
    connDetail: '',
    lastLine: '// no websocket data',
    touch: [createTouchState(), createTouchState(), createTouchState(), createTouchState()],
  }

  const ws = new WsClient(state.wsUrl, {
    onState: (s, detail) => {
      state.conn = s
      state.connDetail = detail ?? ''
      pill.className = pillClass(s)
      pill.textContent = pillText(s)
      connectBtn.textContent = s === 'connected' ? 'disconnect' : 'connect'
      connectBtn.disabled = s === 'connecting'
      statusHint.textContent = state.connDetail
    },
    onRawLine: (line) => {
      state.lastLine = line
      logLine.textContent = line.slice(0, 160)
    },
    onMessage: (msg: WsDeviceToClient, raw) => {
      // For now we only use telemetry; keep raw for debugging.
      if (msg.type === 'telemetry' && msg.touch) {
        applyTouchTelemetry(msg.touch)
      } else if (msg.type === 'error') {
        state.connDetail = msg.message
        statusHint.textContent = msg.message
        state.lastLine = raw
        logLine.textContent = raw.slice(0, 160)
      }
    },
  })

  const sender = createParamSender(ws)

  function applyTouchTelemetry(touch: Record<string, TouchTelemetry>) {
    for (let i = 0; i < 4; i++) {
      const k = `ch${i}`
      const t = touch[k]
      if (!t) continue
      const cur = state.touch[i]
      if (typeof t.raw === 'number') cur.raw = t.raw
      if (typeof t.baseline === 'number') cur.baseline = t.baseline
      if (typeof t.abs_delta === 'number') cur.abs_delta = t.abs_delta
      else cur.abs_delta = Math.abs(cur.raw - cur.baseline)
      if (typeof t.threshold === 'number') cur.threshold = t.threshold
      if (typeof t.intensity === 'number') cur.intensity = t.intensity
      if (typeof t.is_touching === 'boolean') cur.is_touching = t.is_touching

      pushHist(cur.rawHist, cur.raw)
      pushHist(cur.baselineHist, cur.baseline)

      // Update UI for this channel (display-only fields only)
      // Do NOT update slider/input from telemetry to avoid race conditions during user edits
      touchRawEls[i].textContent = `${cur.raw}`
      touchBaseEls[i].textContent = `${cur.baseline}`
      touchDeltaEls[i].textContent = `${cur.abs_delta}`
      touchIntEls[i].textContent = fmtPct(cur.intensity)
      touchBadgeEls[i].classList.toggle('on', cur.is_touching)
      touchBadgeEls[i].textContent = cur.is_touching ? 'touch' : 'idle'
      touchThrEls[i].textContent = `${cur.threshold}`
      // Update slider/input ONLY if significantly different (user hasn't just edited)
      // This prevents UI reverting when user adjusts value while device echoes old value
      const prevSliderVal = parseInt(touchThrSliders[i].value, 10)
      const prevInputVal = parseInt(touchThrInputs[i].value, 10)
      if (Math.abs(cur.threshold - prevSliderVal) > 50 || Math.abs(cur.threshold - prevInputVal) > 50) {
        // Only sync UI if device changed it significantly (not just user typing)
        touchThrSliders[i].value = `${cur.threshold}`
        touchThrInputs[i].value = `${cur.threshold}`
      }
    }
  }

  // UI
  const pill = el('span', { className: pillClass('disconnected'), textContent: 'demo' })
  const statusHint = el('div', { className: 'smallhint', textContent: '' })

  const wsUrlInput = el('input', {
    className: 'input',
    value: state.wsUrl,
    placeholder: 'ws://localhost:8787/ws',
  }) as HTMLInputElement

  const connectBtn = el('button', { className: 'btn', textContent: 'connect' }) as HTMLButtonElement
  connectBtn.addEventListener('click', () => {
    if (ws.getState() === 'connected' || ws.getState() === 'connecting') {
      ws.disconnect()
      return
    }
    state.wsUrl = wsUrlInput.value.trim() || defaultWsUrl()
    ws.setUrl(state.wsUrl)
    ws.connect()
  })

  wsUrlInput.addEventListener('change', () => {
    state.wsUrl = wsUrlInput.value.trim()
    ws.setUrl(state.wsUrl)
  })

  const topbar = el('div', { className: 'topbar' }, [
    el('div', { className: 'topbar-l' }, [
      el('span', { className: 'title', textContent: 'Synthewi · web ui' }),
      pill,
    ]),
    el('div', { className: 'topbar-l' }, [wsUrlInput, connectBtn]),
  ])

  // --- Synth panel (control surface)
  const synthCard = el('div', { className: 'card' })
  const waveBtns = ['sine', 'square', 'saw', 'triangle'].map((name, idx) => {
    const b = el('button', { className: 'wb', textContent: name }) as HTMLButtonElement
    if (idx === 0) b.classList.add('sel')
    b.addEventListener('click', () => {
      for (const other of waveBtns) other.classList.remove('sel')
      b.classList.add('sel')
      sender.flush('synth.wave', idx)
    })
    return b
  })

  const volumeV = el('span', { className: 'vv', textContent: '72%' })
  const volumeS = el('input', { type: 'range', min: '0', max: '100', value: '72' }) as HTMLInputElement
  volumeS.addEventListener('input', () => {
    const v = parseInt(volumeS.value, 10)
    volumeV.textContent = `${v}%`
    sender.set('synth.volume', v)
  })
  volumeS.addEventListener('change', () => {
    sender.flush('synth.volume', parseInt(volumeS.value, 10))
  })

  const muteBtn = el('button', { className: 'btn', textContent: 'mute' }) as HTMLButtonElement
  let muted = false
  muteBtn.addEventListener('click', () => {
    muted = !muted
    muteBtn.textContent = muted ? 'unmute' : 'mute'
    sender.flush('synth.mute', muted)
  })

  synthCard.append(
    el('div', { className: 'sec-label', textContent: 'synth' }),
    el('div', { className: 'fx-grid' }, [
      el('div', { className: 'fx-card' }, [
        el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: 'master' }), muteBtn]),
        el('div', { className: 'sl-row' }, [
          el('label', { textContent: 'vol' }),
          volumeS,
          volumeV,
        ]),
      ]),
      el('div', { className: 'fx-card' }, [
        el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: 'waveform' })]),
        el('div', { className: 'wave-btns' }, waveBtns),
      ]),
    ]),
  )

  function mkToggleFx(name: string, label: string, onByDefault: boolean) {
    const toggle = el('div', { className: `toggle${onByDefault ? ' on' : ''}` }) as HTMLDivElement
    let on = onByDefault
    toggle.addEventListener('click', () => {
      on = !on
      toggle.classList.toggle('on', on)
      sender.flush(`fx.${name}.enabled`, on)
    })
    return { toggle, label }
  }

  const filterFx = mkToggleFx('filter', 'filter', true)
  const distFx = mkToggleFx('dist', 'distortion', false)
  const chorusFx = mkToggleFx('chorus', 'chorus', true)
  const phaserFx = mkToggleFx('phaser', 'phaser', true)
  const delayFx = mkToggleFx('delay', 'delay', true)
  const reverbFx = mkToggleFx('reverb', 'reverb', true)

  function sliderRow(opts: {
    label: string
    min: number
    max: number
    step: number
    value: number
    fmt: (v: number) => string
    onInput: (v: number) => void
    onChange: (v: number) => void
  }) {
    const vEl = el('span', { textContent: opts.fmt(opts.value) })
    const sEl = el('input', {
      type: 'range',
      min: String(opts.min),
      max: String(opts.max),
      step: String(opts.step),
      value: String(opts.value),
    }) as HTMLInputElement

    sEl.addEventListener('input', () => {
      const v = parseFloat(sEl.value)
      vEl.textContent = opts.fmt(v)
      opts.onInput(v)
    })
    sEl.addEventListener('change', () => {
      opts.onChange(parseFloat(sEl.value))
    })

    return el('div', { className: 'sl-row' }, [el('label', { textContent: opts.label }), sEl, vEl])
  }

  const fxGrid = el('div', { className: 'fx-grid' }, [
    el('div', { className: 'fx-card' }, [
      el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: filterFx.label }), filterFx.toggle]),
      sliderRow({
        label: 'cutoff',
        min: 80,
        max: 18000,
        step: 10,
        value: 3000,
        fmt: fmtHz,
        onInput: (v) => sender.set('fx.filter.cutoff_hz', v),
        onChange: (v) => sender.flush('fx.filter.cutoff_hz', v),
      }),
      sliderRow({
        label: 'res',
        min: 0.1,
        max: 20,
        step: 0.1,
        value: 1,
        fmt: (v) => v.toFixed(1),
        onInput: (v) => sender.set('fx.filter.q', v),
        onChange: (v) => sender.flush('fx.filter.q', v),
      }),
    ]),

    el('div', { className: 'fx-card' }, [
      el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: distFx.label }), distFx.toggle]),
      sliderRow({
        label: 'drive',
        min: 1,
        max: 400,
        step: 1,
        value: 80,
        fmt: (v) => `${Math.round(v)}`,
        onInput: (v) => sender.set('fx.dist.drive', v),
        onChange: (v) => sender.flush('fx.dist.drive', v),
      }),
      sliderRow({
        label: 'tone',
        min: 200,
        max: 8000,
        step: 100,
        value: 3000,
        fmt: fmtHz,
        onInput: (v) => sender.set('fx.dist.tone_hz', v),
        onChange: (v) => sender.flush('fx.dist.tone_hz', v),
      }),
      sliderRow({
        label: 'mix',
        min: 0,
        max: 100,
        step: 1,
        value: 60,
        fmt: fmtPct,
        onInput: (v) => sender.set('fx.dist.mix_pct', v),
        onChange: (v) => sender.flush('fx.dist.mix_pct', v),
      }),
    ]),

    el('div', { className: 'fx-card' }, [
      el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: chorusFx.label }), chorusFx.toggle]),
      sliderRow({
        label: 'rate',
        min: 0.1,
        max: 5,
        step: 0.1,
        value: 0.8,
        fmt: (v) => v.toFixed(1),
        onInput: (v) => sender.set('fx.chorus.rate_hz', v),
        onChange: (v) => sender.flush('fx.chorus.rate_hz', v),
      }),
      sliderRow({
        label: 'depth',
        min: 0,
        max: 30,
        step: 1,
        value: 12,
        fmt: (v) => fmtMs(v),
        onInput: (v) => sender.set('fx.chorus.depth_ms', v),
        onChange: (v) => sender.flush('fx.chorus.depth_ms', v),
      }),
      sliderRow({
        label: 'mix',
        min: 0,
        max: 100,
        step: 1,
        value: 50,
        fmt: fmtPct,
        onInput: (v) => sender.set('fx.chorus.mix_pct', v),
        onChange: (v) => sender.flush('fx.chorus.mix_pct', v),
      }),
    ]),

    el('div', { className: 'fx-card' }, [
      el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: phaserFx.label }), phaserFx.toggle]),
      sliderRow({
        label: 'rate',
        min: 0.05,
        max: 4,
        step: 0.05,
        value: 0.3,
        fmt: (v) => v.toFixed(2),
        onInput: (v) => sender.set('fx.phaser.rate_hz', v),
        onChange: (v) => sender.flush('fx.phaser.rate_hz', v),
      }),
      sliderRow({
        label: 'depth',
        min: 0,
        max: 100,
        step: 1,
        value: 70,
        fmt: fmtPct,
        onInput: (v) => sender.set('fx.phaser.depth_pct', v),
        onChange: (v) => sender.flush('fx.phaser.depth_pct', v),
      }),
      sliderRow({
        label: 'mix',
        min: 0,
        max: 100,
        step: 1,
        value: 40,
        fmt: fmtPct,
        onInput: (v) => sender.set('fx.phaser.mix_pct', v),
        onChange: (v) => sender.flush('fx.phaser.mix_pct', v),
      }),
    ]),

    el('div', { className: 'fx-card' }, [
      el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: delayFx.label }), delayFx.toggle]),
      sliderRow({
        label: 'time',
        min: 50,
        max: 800,
        step: 5,
        value: 375,
        fmt: fmtMs,
        onInput: (v) => sender.set('fx.delay.time_ms', v),
        onChange: (v) => sender.flush('fx.delay.time_ms', v),
      }),
      sliderRow({
        label: 'fb',
        min: 0,
        max: 95,
        step: 1,
        value: 45,
        fmt: fmtPct,
        onInput: (v) => sender.set('fx.delay.feedback_pct', v),
        onChange: (v) => sender.flush('fx.delay.feedback_pct', v),
      }),
      sliderRow({
        label: 'mix',
        min: 0,
        max: 100,
        step: 1,
        value: 35,
        fmt: fmtPct,
        onInput: (v) => sender.set('fx.delay.mix_pct', v),
        onChange: (v) => sender.flush('fx.delay.mix_pct', v),
      }),
    ]),

    el('div', { className: 'fx-card' }, [
      el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: reverbFx.label }), reverbFx.toggle]),
      sliderRow({
        label: 'size',
        min: 0.1,
        max: 10,
        step: 0.1,
        value: 3.5,
        fmt: (v) => `${v.toFixed(1)}s`,
        onInput: (v) => sender.set('fx.reverb.size_s', v),
        onChange: (v) => sender.flush('fx.reverb.size_s', v),
      }),
      sliderRow({
        label: 'damp',
        min: 0,
        max: 100,
        step: 1,
        value: 40,
        fmt: fmtPct,
        onInput: (v) => sender.set('fx.reverb.damp_pct', v),
        onChange: (v) => sender.flush('fx.reverb.damp_pct', v),
      }),
      sliderRow({
        label: 'mix',
        min: 0,
        max: 100,
        step: 1,
        value: 30,
        fmt: fmtPct,
        onInput: (v) => sender.set('fx.reverb.mix_pct', v),
        onChange: (v) => sender.flush('fx.reverb.mix_pct', v),
      }),
    ]),
  ])

  // Envelope + bend
  const envCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'envelope + bend' }),
    el('div', { className: 'fx-grid' }, [
      el('div', { className: 'fx-card' }, [
        sliderRow({
          label: 'attack',
          min: 1,
          max: 800,
          step: 1,
          value: 10,
          fmt: fmtMs,
          onInput: (v) => sender.set('env.attack_ms', v),
          onChange: (v) => sender.flush('env.attack_ms', v),
        }),
        sliderRow({
          label: 'release',
          min: 20,
          max: 4000,
          step: 10,
          value: 400,
          fmt: fmtMs,
          onInput: (v) => sender.set('env.release_ms', v),
          onChange: (v) => sender.flush('env.release_ms', v),
        }),
      ]),
      el('div', { className: 'fx-card' }, [
        sliderRow({
          label: 'bend',
          min: 0,
          max: 12,
          step: 0.5,
          value: 2,
          fmt: (v) => `+${v.toFixed(1)}st`,
          onInput: (v) => sender.set('perf.bend_st', v),
          onChange: (v) => sender.flush('perf.bend_st', v),
        }),
        sliderRow({
          label: 'vibrato',
          min: 0,
          max: 30,
          step: 1,
          value: 8,
          fmt: (v) => `${Math.round(v)}¢`,
          onInput: (v) => sender.set('perf.vibrato_cents', v),
          onChange: (v) => sender.flush('perf.vibrato_cents', v),
        }),
      ]),
      el('div', { className: 'fx-card' }, [
        sliderRow({
          label: 'vib rate',
          min: 0.5,
          max: 10,
          step: 0.5,
          value: 5,
          fmt: (v) => `${v.toFixed(1)}Hz`,
          onInput: (v) => sender.set('perf.vibrato_rate_hz', v),
          onChange: (v) => sender.flush('perf.vibrato_rate_hz', v),
        }),
        sliderRow({
          label: 'detune',
          min: 0,
          max: 50,
          step: 1,
          value: 6,
          fmt: (v) => `${Math.round(v)}¢`,
          onInput: (v) => sender.set('perf.detune_cents', v),
          onChange: (v) => sender.flush('perf.detune_cents', v),
        }),
      ]),
    ]),
  ])

  // Keyboard
  const keyEls: HTMLDivElement[] = []
  const keysWrap = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'keyboard' }),
  ])

  const keysGrid = el('div', { className: 'keys-grid' })
  for (let i = 0; i < 7; i++) {
    const k = el('div', { className: 'key' }, [
      el('span', { className: 'kn', textContent: `osc${i}` }),
      el('span', { className: 'kk', textContent: NOTE_NAMES[i] }),
      el('span', { className: 'kf', textContent: BASE_FREQS[i].toFixed(1) }),
    ]) as HTMLDivElement

    const note = 60 + i // C4..B4

    const down = (pressure = 0.5) => {
      k.classList.add('active')
      ws.send({ type: 'note', v: 1, note, gate: 1, pressure })
    }
    const up = () => {
      k.classList.remove('active')
      ws.send({ type: 'note', v: 1, note, gate: 0 })
    }

    k.addEventListener('mousedown', (e) => {
      e.preventDefault()
      down(0.5)
    })
    k.addEventListener('mouseup', (e) => {
      e.preventDefault()
      up()
    })
    k.addEventListener('mouseleave', () => up())

    k.addEventListener('touchstart', (e) => {
      e.preventDefault()
      down(0.5)
    }, { passive: false })
    k.addEventListener('touchend', (e) => {
      e.preventDefault()
      up()
    }, { passive: false })

    keysGrid.append(k)
    keyEls.push(k)
  }
  keysWrap.append(keysGrid, el('div', { className: 'smallhint', textContent: 'keys: A S D F G H J' }))

  const kbDown: Record<string, boolean> = {}
  document.addEventListener('keydown', (e) => {
    if (!(e.key in KEY_MAP)) return
    if (kbDown[e.key]) return
    kbDown[e.key] = true
    const idx = KEY_MAP[e.key]
    keyEls[idx]?.classList.add('active')
    ws.send({ type: 'note', v: 1, note: 60 + idx, gate: 1, pressure: 0.5 })
  })
  document.addEventListener('keyup', (e) => {
    if (!(e.key in KEY_MAP)) return
    kbDown[e.key] = false
    const idx = KEY_MAP[e.key]
    keyEls[idx]?.classList.remove('active')
    ws.send({ type: 'note', v: 1, note: 60 + idx, gate: 0 })
  })

  // Touch tuning
  const touchCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'touch thresholds (4 pads)' }),
  ])

  const touchGrid = el('div', { className: 'touch-grid' })
  const touchRawEls: HTMLSpanElement[] = []
  const touchBaseEls: HTMLSpanElement[] = []
  const touchDeltaEls: HTMLSpanElement[] = []
  const touchIntEls: HTMLSpanElement[] = []
  const touchBadgeEls: HTMLSpanElement[] = []
  const touchThrEls: HTMLSpanElement[] = []
  const touchThrSliders: HTMLInputElement[] = []
  const touchThrInputs: HTMLInputElement[] = []
  const touchSpark: HTMLCanvasElement[] = []

  for (let i = 0; i < 4; i++) {
    const badge = el('span', { className: 'touch-badge', textContent: 'idle' }) as HTMLSpanElement
    const rawV = el('span', { textContent: '0' }) as HTMLSpanElement
    const baseV = el('span', { textContent: '0' }) as HTMLSpanElement
    const delV = el('span', { textContent: '0' }) as HTMLSpanElement
    const intV = el('span', { textContent: '0%' }) as HTMLSpanElement

    const thrV = el('span', { textContent: '100' }) as HTMLSpanElement
    const thrS = el('input', {
      type: 'range',
      min: String(TOUCH_THR_MIN),
      max: String(TOUCH_THR_MAX),
      step: '1',
      value: '100',
    }) as HTMLInputElement
    const thrN = el('input', {
      type: 'number',
      min: String(TOUCH_THR_MIN),
      max: String(TOUCH_THR_MAX),
      step: '1',
      value: '100',
      className: 'input input-num',
    }) as HTMLInputElement
    const thrBtn = el('button', { className: 'btn btn-mini', textContent: 'apply' }) as HTMLButtonElement

    thrS.addEventListener('input', () => {
      const v = parseInt(thrS.value, 10)
      thrV.textContent = `${v}`
      thrN.value = `${v}`
      sender.set(`touch.ch${i}.threshold`, v)
    })
    thrS.addEventListener('change', () => {
      const v = parseInt(thrS.value, 10)
      thrN.value = `${v}`
      sender.flush(`touch.ch${i}.threshold`, v)
    })

    thrN.addEventListener('input', () => {
      const v = parseInt(thrN.value, 10)
      if (!Number.isFinite(v)) return
      const clamped = clamp(v, TOUCH_THR_MIN, TOUCH_THR_MAX)
      thrV.textContent = `${clamped}`
      thrS.value = `${clamped}`
    })

    const applyThreshold = () => {
      const raw = parseInt(thrN.value, 10)
      if (!Number.isFinite(raw)) return
      const v = clamp(raw, TOUCH_THR_MIN, TOUCH_THR_MAX)
      thrN.value = `${v}`
      thrS.value = `${v}`
      thrV.textContent = `${v}`
      sender.flush(`touch.ch${i}.threshold`, v)
    }

    thrBtn.addEventListener('click', applyThreshold)
    thrN.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') applyThreshold()
    })

    const canvas = el('canvas', { className: 'spark' }) as HTMLCanvasElement

    const card = el('div', { className: 'touch-card' }, [
      el('div', { className: 'touch-top' }, [
        el('span', { className: 'touch-name', textContent: `pad ch${i}` }),
        badge,
      ]),
      canvas,
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'raw' }), el('div', {}), rawV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'base' }), el('div', {}), baseV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'delta' }), el('div', {}), delV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'int' }), el('div', {}), intV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'thr' }), thrS, thrV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'set' }), thrN, thrBtn]),
    ])

    touchGrid.append(card)
    touchBadgeEls.push(badge)
    touchRawEls.push(rawV)
    touchBaseEls.push(baseV)
    touchDeltaEls.push(delV)
    touchIntEls.push(intV)
    touchThrEls.push(thrV)
    touchThrSliders.push(thrS)
    touchThrInputs.push(thrN)
    touchSpark.push(canvas)
  }

  touchCard.append(touchGrid)

  function sparkLoop() {
    for (let i = 0; i < 4; i++) {
      drawSpark(touchSpark[i], state.touch[i].rawHist, state.touch[i].baselineHist)
    }
    requestAnimationFrame(sparkLoop)
  }
  requestAnimationFrame(sparkLoop)

  const logLine = el('div', { className: 'logline', textContent: state.lastLine })

  const layout = el('div', { className: 'container' }, [
    topbar,
    statusHint,
    el('div', { className: 'grid2' }, [
      el('div', {}, [
        synthCard,
        el('div', { className: 'card' }, [el('div', { className: 'sec-label', textContent: 'fx' }), fxGrid]),
        envCard,
        keysWrap,
      ]),
      el('div', {}, [
        touchCard,
        el('div', { className: 'sec-label', textContent: 'log' }),
        logLine,
      ]),
    ]),
  ])

  root.replaceChildren(layout)

  // start disconnected; user clicks connect.
}
