import { el, fmtHz, fmtMs, fmtPct, clamp } from '../lib/dom'
import { WsClient, type ConnectionState } from '../proto/wsClient'
import type { TouchTelemetry, SynthState, WsDeviceToClient } from '../proto/types'

const TOUCH_THR_MIN = 1
const TOUCH_THR_MAX = 20000

// Wave names and their AMY wave_id values (SINE=0,PULSE=1,SAW_DOWN=2,SAW_UP=3,TRIANGLE=4)
const WAVE_NAMES: string[]   = ['sine', 'pulse', 'saw↓', 'saw↑', 'tri']
const WAVE_IDS:  number[]    = [0, 1, 2, 3, 4]

type TouchState = Required<TouchTelemetry> & { rawHist: number[]; baselineHist: number[] }
type AppState   = { wsUrl: string; conn: ConnectionState; connDetail: string; lastLine: string; touch: TouchState[] }

function defaultWsUrl() { return 'ws://localhost:8787/ws' }

function createTouchState(): TouchState {
  return { raw: 0, baseline: 0, abs_delta: 0, threshold: 100, intensity: 0, is_touching: false, rawHist: [], baselineHist: [] }
}

function pushHist(arr: number[], v: number, max = 160) {
  arr.push(v)
  if (arr.length > max) arr.splice(0, arr.length - max)
}

function pillClass(s: ConnectionState) {
  return s === 'connected' ? 'pill live' : s === 'error' ? 'pill err' : 'pill demo'
}
function pillText(s: ConnectionState) {
  return s === 'connected' ? 'live' : s === 'connecting' ? 'connecting…' : s === 'error' ? 'error' : 'demo'
}
function nowId() { return `${Date.now()}-${Math.random().toString(16).slice(2)}` }

// ---------------------------------------------------------------------------
// Param sender — coalesces slider spam to ~30 Hz
// ---------------------------------------------------------------------------
function createParamSender(ws: WsClient) {
  const pending = new Map<string, { value: number | boolean | string; lastSentMs: number }>()
  const MIN_INTERVAL = 33

  function tick() {
    const t = Date.now()
    for (const [path, p] of pending) {
      if (t - p.lastSentMs < MIN_INTERVAL) continue
      ws.send({ type: 'set', v: 1, id: nowId(), path, value: p.value })
      p.lastSentMs = t
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

// ---------------------------------------------------------------------------
// Slider factory — pointer-aware, updatable from external state
// ---------------------------------------------------------------------------
type SliderHandle = { el: HTMLElement; setValue(v: number): void }

function mkSlider(
  label: string,
  min: number, max: number, step: number, defaultVal: number,
  fmt: (v: number) => string,
  onInput: (v: number) => void,
  onFlush: (v: number) => void,
): SliderHandle {
  let interacting = false
  const vEl = el('span', { textContent: fmt(defaultVal) })
  const sEl = el('input', {
    type: 'range',
    min: String(min), max: String(max), step: String(step), value: String(defaultVal),
  }) as HTMLInputElement

  sEl.addEventListener('pointerdown', () => { interacting = true })
  sEl.addEventListener('pointerup', () => {
    interacting = false
    const v = parseFloat(sEl.value)
    vEl.textContent = fmt(v)
    onFlush(v)
  })
  sEl.addEventListener('pointercancel', () => { interacting = false })
  sEl.addEventListener('input', () => {
    const v = parseFloat(sEl.value)
    vEl.textContent = fmt(v)
    onInput(v)
  })

  return {
    el: el('div', { className: 'sl-row' }, [el('label', { textContent: label }), sEl, vEl]),
    setValue(v: number) {
      if (interacting) return
      const c = Math.max(min, Math.min(max, v))
      sEl.value = String(c)
      vEl.textContent = fmt(c)
    },
  }
}

// Convert ESP 0-10000 value to display using linear range [displayMin, displayMax]
function espToDisplay(espVal: number, displayMin: number, displayMax: number) {
  return displayMin + (espVal / 10000) * (displayMax - displayMin)
}
// Convert display value to 0-1 float for bridge
function displayToNorm(display: number, displayMin: number, displayMax: number) {
  return (display - displayMin) / (displayMax - displayMin)
}

// ---------------------------------------------------------------------------
// Sparkline renderer
// ---------------------------------------------------------------------------
function drawSpark(canvas: HTMLCanvasElement, raw: number[], baseline: number[]) {
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  const dpr = window.devicePixelRatio || 1
  const W = Math.max(1, Math.floor(canvas.clientWidth  * dpr))
  const H = Math.max(1, Math.floor(canvas.clientHeight * dpr))
  if (canvas.width !== W || canvas.height !== H) { canvas.width = W; canvas.height = H }
  ctx.clearRect(0, 0, W, H)

  const all = raw.concat(baseline)
  let mn = Infinity, mx = -Infinity
  for (const v of all) { if (v < mn) mn = v; if (v > mx) mx = v }
  if (!Number.isFinite(mn) || mn === mx) { mn = 0; mx = 1 }
  const pad = (mx - mn) * 0.08; mn -= pad; mx += pad
  const toY = (v: number) => H - clamp((v - mn) / (mx - mn), 0, 1) * (H - 4) - 2

  ctx.strokeStyle = 'rgba(127,127,127,.18)'; ctx.lineWidth = 1
  ctx.beginPath(); ctx.moveTo(0, H / 2); ctx.lineTo(W, H / 2); ctx.stroke()

  const n = Math.max(raw.length, baseline.length)
  const dx = n > 1 ? W / (n - 1) : W

  ctx.strokeStyle = 'rgba(55,138,221,.55)'; ctx.lineWidth = 1; ctx.beginPath()
  for (let i = 0; i < baseline.length; i++) {
    const x = i * dx, y = toY(baseline[i])
    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y)
  }
  ctx.stroke()

  ctx.strokeStyle = 'rgba(175,169,236,.95)'; ctx.lineWidth = 2; ctx.beginPath()
  for (let i = 0; i < raw.length; i++) {
    const x = i * dx, y = toY(raw[i])
    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y)
  }
  ctx.stroke()
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
export function mountApp(root: HTMLDivElement) {
  const state: AppState = {
    wsUrl: defaultWsUrl(),
    conn: 'disconnected',
    connDetail: '',
    lastLine: '// no websocket data',
    touch: [createTouchState(), createTouchState(), createTouchState(), createTouchState()],
  }

  // ------- synth param slider handles (keyed for state sync) -------
  let activeWaveIdx = 0
  let waveBtns: HTMLButtonElement[] = []

  // Filter
  const sFilterCut = mkSlider('cutoff', 200, 10000, 10, 10000, fmtHz,
    v => sender.set('fx.filter.cutoff', displayToNorm(v, 200, 10000)),
    v => sender.flush('fx.filter.cutoff', displayToNorm(v, 200, 10000)))

  const sFilterRes = mkSlider('res', 0, 0.9, 0.01, 0, v => v.toFixed(2),
    v => sender.set('fx.filter.resonance', displayToNorm(v, 0, 0.9)),
    v => sender.flush('fx.filter.resonance', displayToNorm(v, 0, 0.9)))

  // Reverb
  const sRevAmt = mkSlider('amount', 0, 100, 1, 0, fmtPct,
    v => sender.set('fx.reverb.amount', v / 100),
    v => sender.flush('fx.reverb.amount', v / 100))

  const sRevDec = mkSlider('decay', 0, 100, 1, 50, fmtPct,
    v => sender.set('fx.reverb.decay', v / 100),
    v => sender.flush('fx.reverb.decay', v / 100))

  // Echo
  const sEchoAmt = mkSlider('amount', 0, 100, 1, 0, fmtPct,
    v => sender.set('fx.echo.amount', v / 100),
    v => sender.flush('fx.echo.amount', v / 100))

  const sEchoFb = mkSlider('feedback', 0, 90, 1, 30, fmtPct,
    v => sender.set('fx.echo.feedback', v / 90),
    v => sender.flush('fx.echo.feedback', v / 90))

  // Envelope
  const sEnvAtk = mkSlider('attack', 5, 2000, 5, 5, fmtMs,
    v => sender.set('env.attack', displayToNorm(v, 5, 2000)),
    v => sender.flush('env.attack', displayToNorm(v, 5, 2000)))

  const sEnvRel = mkSlider('release', 50, 5000, 10, 50, fmtMs,
    v => sender.set('env.release', displayToNorm(v, 50, 5000)),
    v => sender.flush('env.release', displayToNorm(v, 50, 5000)))

  // Apply synth state from ESP to all sliders (called on connect and on every telemetry if changed)
  function applySynthState(s: SynthState) {
    // Wave buttons
    const wIdx = WAVE_IDS.indexOf(s.wave_id)
    if (wIdx >= 0 && wIdx !== activeWaveIdx) {
      activeWaveIdx = wIdx
      for (let i = 0; i < waveBtns.length; i++) waveBtns[i].classList.toggle('sel', i === wIdx)
    }
    sFilterCut.setValue(espToDisplay(s.filter_cutoff,    200,  10000))
    sFilterRes.setValue(espToDisplay(s.filter_resonance, 0,    0.9))
    sRevAmt.setValue(   espToDisplay(s.reverb_amount,    0,    100))
    sRevDec.setValue(   espToDisplay(s.reverb_decay,     0,    100))
    sEchoAmt.setValue(  espToDisplay(s.echo_amount,      0,    100))
    sEchoFb.setValue(   espToDisplay(s.echo_feedback,    0,    90))
    sEnvAtk.setValue(   espToDisplay(s.env_attack,       5,    2000))
    sEnvRel.setValue(   espToDisplay(s.env_release,      50,   5000))
  }

  // ------- touch telemetry display elements -------
  const touchRawEls:    HTMLSpanElement[]  = []
  const touchBaseEls:   HTMLSpanElement[]  = []
  const touchDeltaEls:  HTMLSpanElement[]  = []
  const touchIntEls:    HTMLSpanElement[]  = []
  const touchBadgeEls:  HTMLSpanElement[]  = []
  const touchThrEls:    HTMLSpanElement[]  = []
  const touchThrSliders: HTMLInputElement[] = []
  const touchThrInputs:  HTMLInputElement[] = []
  const touchThrInteracting: boolean[]     = [false, false, false, false]
  const touchSpark:     HTMLCanvasElement[] = []

  function applyTouchTelemetry(touch: Record<string, TouchTelemetry>) {
    for (let i = 0; i < 4; i++) {
      const t = touch[`ch${i}`]
      if (!t) continue
      const cur = state.touch[i]
      if (typeof t.raw       === 'number') cur.raw       = t.raw
      if (typeof t.baseline  === 'number') cur.baseline  = t.baseline
      if (typeof t.abs_delta === 'number') cur.abs_delta = t.abs_delta
      else cur.abs_delta = Math.abs(cur.raw - cur.baseline)
      if (typeof t.threshold === 'number') cur.threshold = t.threshold
      if (typeof t.intensity === 'number') cur.intensity = t.intensity
      if (typeof t.is_touching === 'boolean') cur.is_touching = t.is_touching

      pushHist(cur.rawHist, cur.raw)
      pushHist(cur.baselineHist, cur.baseline)

      touchRawEls[i].textContent   = `${cur.raw}`
      touchBaseEls[i].textContent  = `${cur.baseline}`
      touchDeltaEls[i].textContent = `${cur.abs_delta}`
      touchIntEls[i].textContent   = fmtPct(cur.intensity)
      touchBadgeEls[i].classList.toggle('on', cur.is_touching)
      touchBadgeEls[i].textContent = cur.is_touching ? 'touch' : 'idle'
      touchThrEls[i].textContent   = `${cur.threshold}`

      // Update threshold slider/input only when user is not actively dragging
      if (!touchThrInteracting[i]) {
        touchThrSliders[i].value = `${cur.threshold}`
        touchThrInputs[i].value  = `${cur.threshold}`
      }
    }
  }

  // ------- WebSocket client -------
  const ws = new WsClient(state.wsUrl, {
    onState: (s, detail) => {
      state.conn = s; state.connDetail = detail ?? ''
      pill.className = pillClass(s); pill.textContent = pillText(s)
      connectBtn.textContent = s === 'connected' ? 'disconnect' : 'connect'
      connectBtn.disabled = s === 'connecting'
      statusHint.textContent = state.connDetail
    },
    onRawLine: (line) => { state.lastLine = line; logLine.textContent = line.slice(0, 160) },
    onMessage: (msg: WsDeviceToClient, raw) => {
      if (msg.type === 'telemetry') {
        if (msg.touch) applyTouchTelemetry(msg.touch)
        if (msg.synth) applySynthState(msg.synth)
      } else if (msg.type === 'error') {
        state.connDetail = msg.message
        statusHint.textContent = msg.message
        logLine.textContent = raw.slice(0, 160)
      }
    },
  })

  const sender = createParamSender(ws)

  // ------- Topbar -------
  const pill       = el('span', { className: pillClass('disconnected'), textContent: 'demo' })
  const statusHint = el('div', { className: 'smallhint', textContent: '' })
  const wsUrlInput = el('input', { className: 'input', value: state.wsUrl, placeholder: 'ws://localhost:8787/ws' }) as HTMLInputElement
  const connectBtn = el('button', { className: 'btn', textContent: 'connect' }) as HTMLButtonElement

  connectBtn.addEventListener('click', () => {
    if (ws.getState() === 'connected' || ws.getState() === 'connecting') { ws.disconnect(); return }
    state.wsUrl = wsUrlInput.value.trim() || defaultWsUrl()
    ws.setUrl(state.wsUrl); ws.connect()
  })
  wsUrlInput.addEventListener('change', () => { state.wsUrl = wsUrlInput.value.trim(); ws.setUrl(state.wsUrl) })

  const topbar = el('div', { className: 'topbar' }, [
    el('div', { className: 'topbar-l' }, [el('span', { className: 'title', textContent: 'Synthewi · web ui' }), pill]),
    el('div', { className: 'topbar-l' }, [wsUrlInput, connectBtn]),
  ])

  // ------- Synth card (wave + master) -------
  waveBtns = WAVE_NAMES.map((name, idx) => {
    const b = el('button', { className: `wb${idx === 0 ? ' sel' : ''}`, textContent: name }) as HTMLButtonElement
    b.addEventListener('click', () => {
      activeWaveIdx = idx
      for (const o of waveBtns) o.classList.remove('sel')
      b.classList.add('sel')
      sender.flush('synth.wave', WAVE_IDS[idx])
    })
    return b
  })

  const synthCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'synth' }),
    el('div', { className: 'fx-grid' }, [
      el('div', { className: 'fx-card' }, [
        el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: 'waveform' })]),
        el('div', { className: 'wave-btns' }, waveBtns),
      ]),
    ]),
  ])

  // ------- FX card (only AMY-supported effects) -------
  const fxCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'fx' }),
    el('div', { className: 'fx-grid' }, [
      el('div', { className: 'fx-card' }, [
        el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: 'filter' })]),
        sFilterCut.el,
        sFilterRes.el,
      ]),
      el('div', { className: 'fx-card' }, [
        el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: 'reverb' })]),
        sRevAmt.el,
        sRevDec.el,
      ]),
      el('div', { className: 'fx-card' }, [
        el('div', { className: 'fx-head' }, [el('span', { className: 'fx-name', textContent: 'echo' })]),
        sEchoAmt.el,
        sEchoFb.el,
      ]),
    ]),
  ])

  // ------- Envelope card -------
  const envCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'envelope' }),
    el('div', { className: 'fx-grid' }, [
      el('div', { className: 'fx-card' }, [sEnvAtk.el, sEnvRel.el]),
    ]),
  ])

  // ------- Touch threshold cards -------
  const touchCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'touch thresholds' }),
  ])
  const touchGrid = el('div', { className: 'touch-grid' })

  for (let i = 0; i < 4; i++) {
    const badge = el('span', { className: 'touch-badge', textContent: 'idle' }) as HTMLSpanElement
    const rawV  = el('span', { textContent: '0' }) as HTMLSpanElement
    const baseV = el('span', { textContent: '0' }) as HTMLSpanElement
    const delV  = el('span', { textContent: '0' }) as HTMLSpanElement
    const intV  = el('span', { textContent: '0%' }) as HTMLSpanElement
    const thrV  = el('span', { textContent: '100' }) as HTMLSpanElement

    const thrS = el('input', {
      type: 'range', min: String(TOUCH_THR_MIN), max: String(TOUCH_THR_MAX), step: '1', value: '100',
    }) as HTMLInputElement
    const thrN = el('input', {
      type: 'number', min: String(TOUCH_THR_MIN), max: String(TOUCH_THR_MAX), step: '1', value: '100', className: 'input input-num',
    }) as HTMLInputElement
    const thrBtn = el('button', { className: 'btn btn-mini', textContent: 'apply' }) as HTMLButtonElement

    // Pointer tracking — suppress telemetry updates while dragging
    thrS.addEventListener('pointerdown', () => { touchThrInteracting[i] = true })
    thrS.addEventListener('pointerup', () => {
      touchThrInteracting[i] = false
      const v = parseInt(thrS.value, 10)
      thrV.textContent = `${v}`; thrN.value = `${v}`
      sender.flush(`touch.ch${i}.threshold`, v)
    })
    thrS.addEventListener('pointercancel', () => { touchThrInteracting[i] = false })
    thrS.addEventListener('input', () => {
      const v = parseInt(thrS.value, 10)
      thrV.textContent = `${v}`; thrN.value = `${v}`
      sender.set(`touch.ch${i}.threshold`, v)
    })

    thrN.addEventListener('input', () => {
      const v = parseInt(thrN.value, 10)
      if (!Number.isFinite(v)) return
      thrS.value = `${clamp(v, TOUCH_THR_MIN, TOUCH_THR_MAX)}`
      thrV.textContent = thrS.value
    })

    const applyThreshold = () => {
      const v = clamp(parseInt(thrN.value, 10) || 0, TOUCH_THR_MIN, TOUCH_THR_MAX)
      thrN.value = `${v}`; thrS.value = `${v}`; thrV.textContent = `${v}`
      sender.flush(`touch.ch${i}.threshold`, v)
    }
    thrBtn.addEventListener('click', applyThreshold)
    thrN.addEventListener('keydown', (e) => { if (e.key === 'Enter') applyThreshold() })

    const canvas = el('canvas', { className: 'spark' }) as HTMLCanvasElement

    touchGrid.append(el('div', { className: 'touch-card' }, [
      el('div', { className: 'touch-top' }, [el('span', { className: 'touch-name', textContent: `pad ch${i}` }), badge]),
      canvas,
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'raw' }),   el('div', {}), rawV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'base' }),  el('div', {}), baseV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'delta' }), el('div', {}), delV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'int' }),   el('div', {}), intV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'thr' }),   thrS, thrV]),
      el('div', { className: 'sl-row' }, [el('label', { textContent: 'set' }),   thrN, thrBtn]),
    ]))

    touchBadgeEls.push(badge);  touchRawEls.push(rawV);   touchBaseEls.push(baseV)
    touchDeltaEls.push(delV);   touchIntEls.push(intV);   touchThrEls.push(thrV)
    touchThrSliders.push(thrS); touchThrInputs.push(thrN); touchSpark.push(canvas)
  }

  touchCard.append(touchGrid)

  // Sparkline render loop
  ;(function sparkLoop() {
    for (let i = 0; i < 4; i++) drawSpark(touchSpark[i], state.touch[i].rawHist, state.touch[i].baselineHist)
    requestAnimationFrame(sparkLoop)
  })()

  const logLine = el('div', { className: 'logline', textContent: state.lastLine })

  // ------- Layout (no keyboard column) -------
  root.replaceChildren(el('div', { className: 'container' }, [
    topbar,
    statusHint,
    el('div', { className: 'grid2' }, [
      el('div', {}, [synthCard, fxCard, envCard]),
      el('div', {}, [touchCard, el('div', { className: 'sec-label', textContent: 'log' }), logLine]),
    ]),
  ]))
}
