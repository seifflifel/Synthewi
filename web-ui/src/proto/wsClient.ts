import type { WsClientToDevice, WsDeviceToClient } from './types'
import { safeJsonParse } from './types'

export type ConnectionState = 'disconnected' | 'connecting' | 'connected' | 'error'

type Handlers = {
  onState: (state: ConnectionState, detail?: string) => void
  onMessage: (msg: WsDeviceToClient, raw: string) => void
  onRawLine: (line: string) => void
}

export class WsClient {
  private ws: WebSocket | null = null
  private state: ConnectionState = 'disconnected'
  private reconnectTimer: number | null = null
  private reconnectAttempt = 0
  private url: string
  private handlers: Handlers
  private sendQueue: WsClientToDevice[] = []
  private lastPongMs = 0
  private keepAliveTimer: number | null = null

  constructor(url: string, handlers: Handlers) {
    this.url = url
    this.handlers = handlers
  }

  setUrl(url: string) {
    this.url = url
  }

  getState(): ConnectionState {
    return this.state
  }

  connect() {
    this.clearReconnect()
    this.reconnectAttempt = 0
    this.open()
  }

  disconnect() {
    this.clearReconnect()
    this.stopKeepAlive()
    if (this.ws) {
      this.ws.onopen = null
      this.ws.onclose = null
      this.ws.onerror = null
      this.ws.onmessage = null
      this.ws.close()
      this.ws = null
    }
    this.setState('disconnected')
  }

  send(msg: WsClientToDevice) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(msg))
      return
    }
    this.sendQueue.push(msg)
  }

  private flushQueue() {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return
    while (this.sendQueue.length) {
      const m = this.sendQueue.shift()
      if (!m) break
      this.ws.send(JSON.stringify(m))
    }
  }

  private open() {
    this.setState('connecting')

    let ws: WebSocket
    try {
      ws = new WebSocket(this.url)
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      this.setState('error', msg)
      this.scheduleReconnect('bad url')
      return
    }

    this.ws = ws

    ws.onopen = () => {
      this.reconnectAttempt = 0
      this.setState('connected')
      this.flushQueue()
      this.startKeepAlive()
      this.send({ type: 'hello', v: 1, client: 'synthewi-web-ui', ts: Date.now() })
    }

    ws.onmessage = (ev) => {
      const raw = typeof ev.data === 'string' ? ev.data : ''
      if (raw) this.handlers.onRawLine(raw)
      const parsed = safeJsonParse(raw)
      if (!parsed || typeof parsed !== 'object') {
        this.handlers.onMessage({ type: 'error', v: 1, message: 'invalid json' }, raw)
        return
      }
      const msg = parsed as WsDeviceToClient
      if (msg.type === 'pong') this.lastPongMs = Date.now()
      this.handlers.onMessage(msg, raw)
    }

    ws.onerror = () => {
      this.setState('error', 'websocket error')
    }

    ws.onclose = () => {
      this.ws = null
      this.stopKeepAlive()
      const wasConnecting = this.state === 'connecting'
      this.setState('disconnected')
      this.scheduleReconnect(wasConnecting ? 'connect failed' : 'closed')
    }
  }

  private setState(state: ConnectionState, detail?: string) {
    this.state = state
    this.handlers.onState(state, detail)
  }

  private clearReconnect() {
    if (this.reconnectTimer != null) {
      window.clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
  }

  private scheduleReconnect(_reason: string) {
    // Always auto-reconnect for now (simple + good for live iteration)
    this.clearReconnect()
    const attempt = this.reconnectAttempt++
    const delay = Math.min(6000, 250 + attempt * 450)
    this.reconnectTimer = window.setTimeout(() => this.open(), delay)
  }

  private startKeepAlive() {
    this.stopKeepAlive()
    this.lastPongMs = Date.now()
    this.keepAliveTimer = window.setInterval(() => {
      const now = Date.now()
      // send ping every 2s; if no pong in 6s, force reconnect
      if (now - this.lastPongMs > 6000) {
        this.disconnect()
        this.scheduleReconnect('pong timeout')
        return
      }
      this.send({ type: 'ping', v: 1, ts: now })
    }, 2000)
  }

  private stopKeepAlive() {
    if (this.keepAliveTimer != null) {
      window.clearInterval(this.keepAliveTimer)
      this.keepAliveTimer = null
    }
  }
}
