# Media components v1: Video and AudioPlayer

Status: proposed (component library stage 5, [`component-library.md`](../design/component-library.md)
section 8; ships after V1). Builds on the ownership table and `MediaService` contract of
[`media-and-export.md`](../design/media-and-export.md) section 6.1, on
[`custom-elements-v1.md`](custom-elements-v1.md) (the video surface),
[`resource-lifecycle-v1.md`](resource-lifecycle-v1.md) and
[`accessibility-v1.md`](accessibility-v1.md).

Terms: **external texture** is a GPU texture whose pixels come from a platform decoder, not from
Tessera's own draw calls. **Surface** is the element that shows the decoded frames. **Handle** is
the `MediaHandle` of an open source. **Pacer** is the frame pacer that schedules frames
([`dirty-rendering.md`](../design/dirty-rendering.md)). See also the [glossary](../reference/glossary.md).

## 1. Goals and non-goals

Goals:

- `Video` and `AudioPlayer` as library components with typed props, events and a ref, on top of
  `MediaService`; no decoder, audio device or OS handle is visible to application code.
- Static UI still idles: a paused or hidden player causes zero presents and no decode.
- Deterministic tests with a fake decoder and the manual clock.

Non-goals (not in this spec): DRM, adaptive streaming (HLS, DASH), live capture (camera,
microphone, screen recording), editing, picture-in-picture, casting. Each needs its own spec.

## 2. Components

| Component | Draws | Built from |
|---|---|---|
| `Video` | the surface, optional poster, optional controls | `VideoSurface` custom element + `MediaService` |
| `AudioPlayer` | controls only (no surface) | `MediaService` |

`VideoSurface` is a custom element ([ADR-028](../../decisions/ADR-028-custom-native-elements.md))
that owns the external texture and nothing else. Resizing, occlusion and device loss follow the
custom element rules; after device loss the surface shows the poster until the next decoded frame.

## 3. Props

| Prop | Type | Default | Notes |
|---|---|---|---|
| `src` | `MediaSource` (asset id, file path, or `https` URL) | required | A URL goes through the network stack of [`networking-v1.md`](networking-v1.md) |
| `playing` / `defaultPlaying` | bool | `false` | Controlled / uncontrolled pair; never both |
| `position` / `defaultPosition` | seconds | `0` | Same pair rule; controlled position seeks on change |
| `volume`, `muted` | `0..1`, bool | `1`, `false` | |
| `loop` | bool | `false` | |
| `rate` | number | `1` | Allowed `0.5..2`; audio pitch is kept |
| `preload` | `none`, `metadata`, `auto` | `metadata` | `none` opens the handle on first play |
| `fit` | `contain`, `cover`, `fill` | `contain` | `Video` only |
| `poster` | image source | none | Shown before the first frame and after device loss |
| `controls` | bool | `true` | Library chrome built from parts (below); `false` for custom controls |
| `label` | string (i18n key) | required | Accessible name |

`autoplay` is not a prop: starting playback is `playing`. A source with an audio track never
starts by itself; set `muted` first (the same rule as browsers) so a screen never makes sound
without user intent ([`media-and-export.md`](../design/media-and-export.md) audio focus rule).

## 4. Events

| Event | Payload | Rule |
|---|---|---|
| `onReady` | `{duration, width, height}` | Once per `src` |
| `onPlayingChange` | `{playing}` | Includes platform interruptions (audio focus loss) |
| `onTimeUpdate` | `{position, duration, buffered}` | At most 4 per second, and not at all while paused |
| `onBuffering` | `{buffering}` | |
| `onEnded` | none | Not fired when `loop` is true |
| `onVolumeChange` | `{volume, muted}` | |
| `onError` | `MediaError` | Section 7 |

Events are asynchronous completions drained by the runtime loop; none runs on a decoder thread.

## 5. Ref

`play()`, `pause()`, `seek(seconds)` return the `Task<Status>` of `MediaService`. They are the
only imperative surface; every other state is a prop.

## 6. Lifecycle, threading, power

- The handle opens when the component mounts (or on first play for `preload="none"`) and closes on
  unmount. Closing cancels pending opens and seeks; a completion arriving after unmount is dropped.
- Decode and audio output run off the UI thread; frames arrive as external-texture updates. No
  frame is copied through the CPU on the frame path.
- While `playing`, the pacer wakes at the video frame cadence, capped to the display rate. Paused,
  ended, hidden or occluded: no wake, no decode, no present (render-on-demand).
- Background playback and audio focus follow platform policy. Focus loss pauses (or ducks for
  transient loss); the player never resumes without user intent.

## 7. Errors

`MediaError{code, message}` with codes `source_not_found`, `network`, `unsupported_format`,
`decode_failed`, `permission_denied`, `device_lost`. All are recoverable by changing `src` or
retrying `play()`; none throws ([`error-model-v1.md`](error-model-v1.md)). The library chrome shows
an explicit error state with a retry button, not a blank surface.

## 8. Styling parts and accessibility

Parts ([ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md)):
`surface`, `poster`, `controls`, `play-button`, `seek-bar`, `seek-thumb`, `time`, `volume`,
`fullscreen-button`, `error`. Part names are public API.

- The component has role `group` with the `label`; the controls follow the toggle/slider
  accessibility contract (`play-button` is a toggle, `seek-bar` is a slider with value text such as
  "1:05 of 3:20").
- Keyboard when focused: Space toggles, Left/Right seek 5 s, Up/Down volume, `m` mutes, `f`
  toggles fullscreen where the platform has it.
- Reduced motion: control fades become cross-fades with no movement; auto-hiding controls stay
  visible while focus is inside them.
- Captions: text tracks are not part of this spec (open question 3); until then the app draws
  captions as ordinary `Text` over the surface.

## 9. Testing

- Unit: state machine (idle, opening, ready, playing, buffering, ended, error) against a fake
  decoder and audio sink with a deterministic playback clock; interruption and unmount races.
- Headless: the semantics snapshot shows role, label, slider value text; scripted keyboard play,
  pause, seek.
- Golden: `Video` with the fake decoder emitting a fixed test pattern per frame index; default,
  dark and high-contrast themes, with and without controls.
- Power: a paused player records zero presents over N idle ticks.

## 10. Open questions

1. **Codec set and patent/licensing stance** (H.264 and AAC are platform-decoded; which formats
   are guaranteed on every platform, and what the Linux fallback is).
2. **Adaptive streaming**: HLS first, DASH, or neither in the first release.
3. **Text tracks** (WebVTT subset) and their accessibility mapping.
4. **Fullscreen** semantics per platform and its interaction with window chrome.
5. **Web target**: whether `Video` maps to the browser media element on Wasm instead of an
   external texture ([ADR-046](../../decisions/ADR-046-web-target-for-demos.md) if it covers this).
