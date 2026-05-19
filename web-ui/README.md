# Synthewi Web UI

Single-page Vite + TypeScript UI for live tuning + synth controls.

## Dev

```powershell
cd web-ui
npm install
npm run dev
```

Then connect the UI to your ESP32 WebSocket URL (default is `ws://192.168.4.1/ws`).

## Notes
- UI sends coalesced slider updates (~30Hz) to avoid flooding the device.
- All device communication is JSON over WebSocket.
