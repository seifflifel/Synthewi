import { el, fmtHz, fmtMs, fmtPct, clamp } from '../lib/dom'
import { WsClient, type ConnectionState } from '../proto/wsClient'
import type { TouchTelemetry, SynthState, WsDeviceToClient } from '../proto/types'

const TOUCH_THR_MIN = 1
const TOUCH_THR_MAX = 20000

// Wave names and their engine wave_id values.
// pulse=1 (duty 0.2, nasal), saw=2, tri=4, square=5 (duty 0.5, fuller).
// sine(0) and saw_up(3) removed — sine sounds thin, saw_up identical to saw_down.
const WAVE_NAMES: string[] = ['pulse', 'saw↓', 'tri', 'square']
const WAVE_IDS:  number[]  = [1, 2, 4, 5]

// Filter type names and values (maps to AMY_ENGINE_FILTER_*)
const FTYPE_NAMES: string[] = ['lpf', 'bpf', 'hpf']
const FTYPE_IDS:   number[] = [0, 1, 2]

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
    touch: Array.from({ length: 8 }, createTouchState),
  }

  // ------- synth param slider handles (keyed for state sync) -------
  let activeWaveIdx  = 0
  let activeFtypeIdx = 0
  let waveBtns:  HTMLButtonElement[] = []
  let ftypeBtns: HTMLButtonElement[] = []

  // ------- PATCH mode state -------
  type SynthModeId = 0 | 1 | 2  // 0=CUSTOM 1=JUNO 2=DX7
  let activeMode:  SynthModeId = 0
  let activePatch: number       = 0  // 0-127 within current bank
  const PATCH_BANK_SIZE = 128
  const MODE_NAMES = ['CUSTOM', 'JUNO', 'DX7'] as const

  // Filter
  const sFilterCut = mkSlider('cutoff', 200, 10000, 10, 10000, fmtHz,
    v => sender.set('fx.filter.cutoff', displayToNorm(v, 200, 10000)),
    v => sender.flush('fx.filter.cutoff', displayToNorm(v, 200, 10000)))

  const sFilterRes = mkSlider('res', 0, 0.9, 0.01, 0, v => v.toFixed(2),
    v => sender.set('fx.filter.resonance', displayToNorm(v, 0, 0.9)),
    v => sender.flush('fx.filter.resonance', displayToNorm(v, 0, 0.9)))

  const sGlide = mkSlider('glide', 0, 500, 5, 0, fmtMs,
    v => sender.set('synth.glide', displayToNorm(v, 0, 500)),
    v => sender.flush('synth.glide', displayToNorm(v, 0, 500)))

  const sPresDepth = mkSlider('pres depth', 0, 8000, 50, 0, fmtHz,
    v => sender.set('touch.pressure.depth', displayToNorm(v, 0, 8000)),
    v => sender.flush('touch.pressure.depth', displayToNorm(v, 0, 8000)))

  const sPresRange = mkSlider('pres range', 1.0, 5.0, 0.1, 2.0, v => `${v.toFixed(1)}×`,
    v => sender.set('touch.pressure.range', v),
    v => sender.flush('touch.pressure.range', v))

  // Filter envelope
  const sFenvDepth = mkSlider('depth', 0, 8000, 50, 0, fmtHz,
    v => sender.set('fx.filter.env.depth', displayToNorm(v, 0, 8000)),
    v => sender.flush('fx.filter.env.depth', displayToNorm(v, 0, 8000)))

  const sFenvDecay = mkSlider('decay', 5, 2000, 5, 205, fmtMs,
    v => sender.set('fx.filter.env.decay', displayToNorm(v, 5, 2000)),
    v => sender.flush('fx.filter.env.decay', displayToNorm(v, 5, 2000)))

  // LFO
  const sLfoRate = mkSlider('rate', 0.1, 10, 0.1, 2, v => `${v.toFixed(1)} Hz`,
    v => sender.set('fx.lfo.rate', displayToNorm(v, 0.1, 10)),
    v => sender.flush('fx.lfo.rate', displayToNorm(v, 0.1, 10)))

  const sLfoDepth = mkSlider('depth', 0, 5000, 50, 0, fmtHz,
    v => sender.set('fx.lfo.depth', displayToNorm(v, 0, 5000)),
    v => sender.flush('fx.lfo.depth', displayToNorm(v, 0, 5000)))

  // Envelope
  const sEnvAtk = mkSlider('attack', 5, 2000, 5, 5, fmtMs,
    v => sender.set('env.attack', displayToNorm(v, 5, 2000)),
    v => sender.flush('env.attack', displayToNorm(v, 5, 2000)))

  const sEnvRel = mkSlider('release', 50, 5000, 10, 50, fmtMs,
    v => sender.set('env.release', displayToNorm(v, 50, 5000)),
    v => sender.flush('env.release', displayToNorm(v, 50, 5000)))

  // Apply synth state from ESP to all sliders (called on connect and on telemetry change)
  function applySynthState(s: SynthState) {
    const wIdx = WAVE_IDS.indexOf(s.wave_id)
    if (wIdx >= 0 && wIdx !== activeWaveIdx) {
      activeWaveIdx = wIdx
      for (let i = 0; i < waveBtns.length; i++) waveBtns[i].classList.toggle('sel', i === wIdx)
    }
    const fIdx = FTYPE_IDS.indexOf(s.filter_type ?? 0)
    if (fIdx >= 0 && fIdx !== activeFtypeIdx) {
      activeFtypeIdx = fIdx
      for (let i = 0; i < ftypeBtns.length; i++) ftypeBtns[i].classList.toggle('sel', i === fIdx)
    }
    sFilterCut.setValue(espToDisplay(s.filter_cutoff,    200,  10000))
    sFilterRes.setValue(espToDisplay(s.filter_resonance, 0,    0.9))
    sFenvDepth.setValue(espToDisplay(s.filter_env_depth ?? 0, 0, 8000))
    sFenvDecay.setValue(espToDisplay(s.filter_env_decay ?? 1000, 5, 2000))
    sLfoRate.setValue(  espToDisplay(s.lfo_rate  ?? 2000, 0.1, 10))
    sLfoDepth.setValue( espToDisplay(s.lfo_depth ?? 0,    0,   5000))
    sEnvAtk.setValue(   espToDisplay(s.env_attack,       5,    2000))
    sEnvRel.setValue(   espToDisplay(s.env_release,      50,   5000))
    if (s.pressure_depth !== undefined)
      sPresDepth.setValue(espToDisplay(s.pressure_depth, 0, 8000))
    if (s.glide !== undefined)
      sGlide.setValue(espToDisplay(s.glide, 0, 500))
  }

  // ------- mode switch (called when mode buttons are clicked) -------
  function applyModeVisibility() {
    const isCustom = activeMode === 0
    customControls.style.display  = isCustom ? '' : 'none'
    patchControls.style.display   = isCustom ? 'none' : ''
    patchBankLabel.textContent    = MODE_NAMES[activeMode]
    patchNumDisplay.textContent   = String(activePatch + 1).padStart(3, '0')
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
  const touchThrInteracting: boolean[]     = Array(8).fill(false)
  const touchThrLastSentMs: number[]       = Array(8).fill(0)
  const THR_HOLD_MS = 1500 // block telemetry overwrites for this long after sending a command
  const touchSpark:     HTMLCanvasElement[] = []

  function applyTouchTelemetry(touch: Record<string, TouchTelemetry>) {
    for (let i = 0; i < 8; i++) {
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

      // Change-detection: only write to DOM if value actually changed (avoids 30Hz reflow)
      const setT = (el: HTMLSpanElement, v: string) => { if (el.textContent !== v) el.textContent = v }
      setT(touchRawEls[i],   `${cur.raw}`)
      setT(touchBaseEls[i],  `${cur.baseline}`)
      setT(touchDeltaEls[i], `${cur.abs_delta}`)
      setT(touchIntEls[i],   fmtPct(cur.intensity))
      const touchStr = cur.is_touching ? 'touch' : 'idle'
      touchBadgeEls[i].classList.toggle('on', cur.is_touching)
      setT(touchBadgeEls[i] as HTMLSpanElement, touchStr)
      setT(touchThrEls[i],   `${cur.threshold}`)

      // Only update slider from telemetry if not interacting AND command hold has expired
      if (!touchThrInteracting[i] && Date.now() - touchThrLastSentMs[i] > THR_HOLD_MS) {
        const thrStr = `${cur.threshold}`
        if (touchThrSliders[i].value !== thrStr) touchThrSliders[i].value = thrStr
        if (touchThrInputs[i].value  !== thrStr) touchThrInputs[i].value  = thrStr
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

  // ------- Mode row (CUSTOM / JUNO / DX7) -------
  let modeBtns: HTMLButtonElement[] = []
  let patchBankLabel  = el('span', { className: 'fx-name', textContent: 'JUNO' })
  let patchNumDisplay = el('span', { className: 'patch-num', textContent: '001' })

  function sendMode(mode: SynthModeId) {
    activeMode = mode
    modeBtns.forEach((b, i) => b.classList.toggle('sel', i === mode))
    sender.flush('synth.mode', mode)
    applyModeVisibility()
  }

  function sendPatch(delta: number) {
    activePatch = (activePatch + delta + PATCH_BANK_SIZE) % PATCH_BANK_SIZE
    patchNumDisplay.textContent = String(activePatch + 1).padStart(3, '0')
    sender.flush('synth.patch', activePatch)
  }

  modeBtns = (['CUSTOM', 'JUNO', 'DX7'] as const).map((name, idx) => {
    const b = el('button', {
      className: `wb${idx === 0 ? ' sel' : ''}`,
      textContent: name,
    }) as HTMLButtonElement
    b.addEventListener('click', () => sendMode(idx as SynthModeId))
    return b
  })

  const prevPatchBtn = el('button', { className: 'btn btn-mini', textContent: '◀' }) as HTMLButtonElement
  const nextPatchBtn = el('button', { className: 'btn btn-mini', textContent: '▶' }) as HTMLButtonElement
  prevPatchBtn.addEventListener('click', () => sendPatch(-1))
  nextPatchBtn.addEventListener('click', () => sendPatch(+1))

  // placeholder refs — filled after card construction
  let customControls: HTMLElement
  let patchControls: HTMLElement

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

  // ------- Synth card (wave + filter type) -------
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

  ftypeBtns = FTYPE_NAMES.map((name, idx) => {
    const b = el('button', { className: `wb${idx === 0 ? ' sel' : ''}`, textContent: name }) as HTMLButtonElement
    b.addEventListener('click', () => {
      activeFtypeIdx = idx
      for (const o of ftypeBtns) o.classList.remove('sel')
      b.classList.add('sel')
      sender.flush('fx.filter.type', FTYPE_IDS[idx])
    })
    return b
  })

  // ------- OSC card -------
  const oscCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'osc' }),
    el('div', { className: 'wave-btns' }, waveBtns),
    sGlide.el,
  ])

  // ------- FILTER card -------
  const filterCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'filter' }),
    el('div', { className: 'wave-btns' }, ftypeBtns),
    sFilterCut.el,
    sFilterRes.el,
    sPresDepth.el,
    sPresRange.el,
  ])

  // ------- MOD card (LFO + filter env) -------
  const modCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'mod' }),
    el('div', { className: 'sec-label', style: 'margin-top:6px', textContent: 'lfo' }),
    sLfoRate.el,
    sLfoDepth.el,
    el('div', { className: 'sec-label', style: 'margin-top:8px', textContent: 'filter env' }),
    sFenvDepth.el,
    sFenvDecay.el,
  ])

  // ------- ENV card -------
  const envCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'env' }),
    sEnvAtk.el,
    sEnvRel.el,
  ])

  // ------- Touch threshold cards -------
  const touchCard = el('div', { className: 'card' }, [
    el('div', { className: 'sec-label', textContent: 'touch thresholds' }),
  ])
  const touchGrid = el('div', { className: 'touch-grid' })

  for (let i = 0; i < 8; i++) {
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

    const sendThreshold = (v: number) => {
      touchThrLastSentMs[i] = Date.now()
      thrV.textContent = `${v}`; thrN.value = `${v}`
      sender.flush(`touch.ch${i}.threshold`, v)
      // Hold the interacting lock so incoming telemetry can't snap the slider back
      // before the command reaches the ESP and the new threshold is reflected.
      setTimeout(() => { touchThrInteracting[i] = false }, 200)
    }

    thrS.addEventListener('pointerdown', () => { touchThrInteracting[i] = true })
    thrS.addEventListener('pointerup',   () => { sendThreshold(parseInt(thrS.value, 10)) })
    thrS.addEventListener('pointercancel', () => { touchThrInteracting[i] = false })
    thrS.addEventListener('input', () => {
      const v = parseInt(thrS.value, 10)
      thrV.textContent = `${v}`; thrN.value = `${v}`
    })

    thrN.addEventListener('input', () => {
      const v = parseInt(thrN.value, 10)
      if (!Number.isFinite(v)) return
      thrS.value = `${clamp(v, TOUCH_THR_MIN, TOUCH_THR_MAX)}`
      thrV.textContent = thrS.value
    })

    const applyThreshold = () => {
      const v = clamp(parseInt(thrN.value, 10) || 0, TOUCH_THR_MIN, TOUCH_THR_MAX)
      thrS.value = `${v}`
      sendThreshold(v)
    }
    thrBtn.addEventListener('click', applyThreshold)
    thrN.addEventListener('keydown', (e) => { if (e.key === 'Enter') applyThreshold() })

    const canvas = el('canvas', { className: 'spark' }) as HTMLCanvasElement

    touchGrid.append(el('div', { className: 'touch-card' }, [
      el('div', { className: 'touch-top' }, [
        el('span', { className: 'touch-name' }, [
          el('span', { textContent: `ch${i} ` }),
          el('span', { className: 'touch-note', textContent: ['C4','D4','E4','G4','A4','C5','D5','E5'][i] }),
        ]),
        badge,
      ]),
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
    for (let i = 0; i < 8; i++) drawSpark(touchSpark[i], state.touch[i].rawHist, state.touch[i].baselineHist)
    requestAnimationFrame(sparkLoop)
  })()

  const logLine = el('div', { className: 'logline', textContent: state.lastLine })

  const modeRow = el('div', { className: 'mode-row' }, [
    el('div', { className: 'wave-btns' }, modeBtns),
  ])

  customControls = el('div', { className: 'controls-row' }, [oscCard, filterCard, modCard, envCard])

  patchControls = el('div', { className: 'card', style: 'display:none' }, [
    el('div', { className: 'sec-label' }, [patchBankLabel]),
    el('div', { className: 'patch-row' }, [
      prevPatchBtn,
      patchNumDisplay,
      nextPatchBtn,
    ]),
  ])

  root.replaceChildren(el('div', { className: 'container' }, [
    topbar,
    statusHint,
    modeRow,
    customControls,
    patchControls,
    touchCard,
    el('div', { className: 'sec-label', textContent: 'log' }),
    logLine,
  ]))
}
