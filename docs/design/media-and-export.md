# Media playback, printing, and export

Status: design (optional, after V1; V1 is the first-release scope referred to by [AGENTS.md rule 8](../../AGENTS.md#non-negotiables), and no release is assigned to this work). Not core renderer work; decide per app/product.

A UI framework does not need to become a media engine, but it must define the seams so apps can
embed playback, print, and export without leaking OS/GPU types into components.

## 1. Media playback

- Video/audio are **services** ([`services.md`](services.md)) or **custom elements** ([`custom-elements-v1.md`](../specs/custom-elements-v1.md)), not built-in scene nodes.
- Video surface: platform decoder/texture or RHI (render hardware interface, [glossary](../reference/glossary.md)) external texture; no raw OS handle in UI.
- Audio: optional service (miniaudio or platform); no blocking on the UI thread.
- Playback state (position, duration, buffering, error) is exposed as a resource/state; UI
  controls are normal components.
- DRM (digital rights management)/platform-protected playback is app/platform-specific; document supported paths.
- Background/audio focus/now-playing integration follows platform policy.
- Power: pause when hidden/occluded; no continuous decode without a visible/active reason.

## 2. Printing and PDF

- Printing is a platform-shell service ([`platform-shell-v1.md`](../specs/platform-shell-v1.md)): enumerate printers, page setup, print dialog, status.
- Page layout is app-owned; the framework supplies text/layout primitives and a render target.
- PDF export may use a platform API or an optional library; output must be deterministic where
  tested.
- Print/export runs off the UI thread; progress/cancel are first-class.
- Security: no arbitrary file paths; user-selected destinations only.
- Not in V1 unless a target app requires it.

## 3. Image/vector export

- Snapshot a subtree/window to a texture/PNG/SVG-path export.
- Use the software/golden path (CPU-rendered deterministic reference output, see [glossary](../reference/glossary.md)) for deterministic output; GPU readback only for diagnostics.
- Bound dimensions/bytes; no unbounded full-scene export.
- Text/vector export should preserve shaping/bidi; when it cannot, the export falls back to raster explicitly (the conditions are not specified).
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

## 6.1 Detail design

Ownership:

| Capability | Owner | Boundary |
|---|---|---|
| Video decode/surface | platform media service or custom element | no raw decoder/OS handle in UI |
| Audio playback/focus | optional audio service | no blocking in UI thread |
| Playback controls/state | normal components + resource state | no hidden singleton |
| Print/PDF | platform shell service + app layout | output path user-selected |
| Image/vector export | renderer/software readback API | no GPU readback in frame loop |

Playback contract:

```cpp
struct PlaybackState {
    bool playing;
    bool buffering;
    double position_s;
    double duration_s;
    float volume;
    std::optional<Error> error;
};

class MediaService {
public:
    Task<Result<MediaHandle>> open(MediaSource, MediaOptions);
    Task<Status> play(MediaHandle);
    Task<Status> pause(MediaHandle);
    Task<Status> seek(MediaHandle, double position_s);
};
```

- Video surface is an external texture/RHI texture or a custom element; resizing/occlusion/
  device-loss follow the renderer/custom element rules.
- Audio focus/interruption is a platform event; pause/duck per policy; no auto-resume without
  user intent.
- Background playback is platform-policy-gated; no continuous decode when hidden.

Export/print:

- Print/PDF lays out through the normal text/layout stack into a render target; output is
  deterministic in software/golden mode.
- Export runs off the UI thread with cancel/progress; bounded dimensions/bytes.
- Image export uses readback only for explicit user action, not per frame.
- Paths are user-selected/sandboxed; no traversal/arbitrary writes; redact metadata.

Failure/edge cases:

- Unsupported codec/format -> error state; no crash/fallback decoder.
- Device loss/occlusion -> recreate/pause; resume only if policy allows.
- Print/export cancel -> delete partial output; no corrupt file.
- PDF/vector font fallback -> deterministic embedded/pinned fonts; cases where this is not possible are not specified.

Gap-closure decisions:

- **G-27/Q-25** (gap and question ids in [`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md), where G-27 is marked Deferred): media/print/PDF/export are optional app/platform packages, not core. Video uses
  an external texture or custom element; audio focus/interruption is platform policy; playback
  state is explicit. Print/PDF/export require deterministic software layout for tests. No
  default media engine or DRM stack ships in V1.

## 7. Deferred/open questions

- DRM/protected playback.
- PDF/print library choice and licensing.
- SVG/vector export fidelity.
- Background audio/video policy per platform.
- Whether an optional `tessera-media` package ships after V1 (no decision yet).
