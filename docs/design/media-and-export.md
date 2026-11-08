# Media playback, printing, and export

Status: design (optional, post-V1). Not core renderer work; decide per app/product.

A UI framework does not need to become a media engine, but it must define the seams so apps can
embed playback, print, and export without leaking OS/GPU types into components.

## 1. Media playback

- Video/audio are **services** or **custom elements**, not built-in scene nodes.
- Video surface: platform decoder/texture or RHI external texture; no raw OS handle in UI.
- Audio: optional service (miniaudio or platform); no blocking on the UI thread.
- Playback state (position, duration, buffering, error) is exposed as a resource/state; UI
  controls are normal components.
- DRM/platform-protected playback is app/platform-specific; document supported paths.
- Background/audio focus/now-playing integration follows platform policy.
- Power: pause when hidden/occluded; no continuous decode without a visible/active reason.

## 2. Printing and PDF

- Printing is a platform-shell service: enumerate printers, page setup, print dialog, status.
- Page layout is app-owned; the framework supplies text/layout primitives and a render target.
- PDF export may use a platform API or an optional library; output must be deterministic where
  tested.
- Print/export runs off the UI thread; progress/cancel are first-class.
- Security: no arbitrary file paths; user-selected destinations only.
- Not in V1 unless a target app requires it.

## 3. Image/vector export

- Snapshot a subtree/window to a texture/PNG/SVG-path export.
- Use the software/golden path for deterministic output; GPU readback only for diagnostics.
- Bound dimensions/bytes; no unbounded full-scene export.
- Text/vector export should preserve shaping/bidi where possible; raster fallback is explicit.
- Export is app-facing API, not a hidden side effect of screenshots.

## 4. Platform mapping

| Capability | Windows | macOS | Linux | Android | iOS |
|---|---|---|---|---|---|
| Video surface | Media Foundation/D3D interop | AVFoundation/Metal | GStreamer/VA-API (optional) | MediaCodec/Surface | AVPlayer |
| Audio | WASAPI | CoreAudio | PipeWire/ALSA | AudioTrack | AVAudioSession |
| Print | Win32 print | NSPrintOperation | CUPS/portal | PrintManager | UIPrintInteraction |
| PDF | platform/optional | platform | optional | PdfDocument | UIGraphicsPDF |

These are app/platform integrations; the core RHI does not depend on media APIs.

## 5. Security and privacy

- Media files/URLs are untrusted: validate type/size, no arbitrary codecs/plugins.
- No autoplay with sound without user intent; respect platform policy.
- Printing/export destinations are user-selected; no silent file writes.
- Redact media paths/metadata in diagnostics.

## 6. Testing

- Fake decoder/audio sink; deterministic playback clock.
- Video surface resize/occlusion/device loss; audio focus interruption.
- Print/PDF layout snapshots; cancel/error.
- Export dimensions/bytes/determinism; no PII.
- Headless/no-GPU fallback.

## 7. Open questions

- Whether to ship a default media service or leave it to platform packages.
- DRM/protected playback.
- PDF/print library choice and licensing.
- SVG/vector export fidelity.
- Background audio/video policy per platform.
