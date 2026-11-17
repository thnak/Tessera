# Studio device profiles, platform events and live edit v1

Status: accepted (design-time, V1-M4/D; owner accepted the spec on 2026-11-17, with the working choices of
section 9 below). Owner direction, 2026-11-17 (paraphrased from the owner):
the emulator's device is fixed today; changing the device must be allowed to restart the app state, but
the platform's events must exist so the UI can simulate them (an Android device needs simulated
hardware buttons, and the runtime simulates the events); and the developer must not have to press Run
after editing code (hot reload).
Related: [designer-studio-v1.md](designer-studio-v1.md) (sections 5, 6 and 7 are extended here),
[os-events-v1.md](os-events-v1.md) (the normalized event model), [designer-protocol-v1.md](designer-protocol-v1.md),
[safe-area-v1.md](safe-area-v1.md), [navigation-v1.md](navigation-v1.md), [startup-api-v1.md](startup-api-v1.md),
[../design/designer.md](../design/designer.md) section 4 (hot reload).

## 1. What exists today, and what does not

Checked against the repository on 2026-11-17 (the live page is build `6ca19240`, #439).

| Piece | State |
|---|---|
| Three-column workspace, file tree, file tabs | landed (#439) |
| Viewport as a list of sizes, DPR, theme, fold posture | landed; **no notion of a device or platform** |
| The preview starts only after **Run** (`Ctrl+Enter`); the canvas says "not started; press Run" | landed; this is what section 6 removes |
| The state-carrying re-evaluation across an edit (P4 rule, `scene_state`) | landed in the engine; no automatic trigger |
| `OsEventType` lifecycle, theme, locale, text scale, orientation, fold, power, memory events and the `SyntheticEventInjector` | landed (`tessera/hal/os_event.hpp`, `synthetic_events.hpp`); not reachable from the browser |
| `NavigationController::back()` and overlay `dismiss_on_back` | landed in the framework; **nothing delivers a system Back to them** |
| Hardware or system buttons as normalized events: Back, Home, Recents, Volume, Power | **absent**. `os-events-v1.md` section 7 has one row, "Keyboard/back, `KeyEvent`, `OnBackInvokedCallback` -> input events", and no `OsEventType` or key code exists for them |
| A message to inject a platform event into the viewer | **absent**. The protocol has no such message, and `designer-studio-v1.md` section 4 says the studio adds none |
| Process death and relaunch with saved state | not modeled |

## 2. Device profile

A **device profile** is data, like a skin ([designer-studio-v1.md](designer-studio-v1.md) section 5.2),
and replaces the bare size list. One directory per profile; no code.

```json
{
  "schemaVersion": 1,
  "name": "Generic Android phone",
  "category": "mobile",
  "os": "android",
  "size": { "width": 412, "height": 915 },
  "dpr": 2.625,
  "navigation": "three-button",
  "controls": ["back", "home", "recents", "volumeUp", "volumeDown", "power"],
  "events": ["suspend", "resume", "screenOff", "screenOn", "theme", "locale", "textScale",
             "orientation", "ime", "memoryPressure", "batterySaver", "processDeath"]
}
```

- `navigation` is `three-button`, `gesture` (Android and iOS home indicator) or `none` (desktop,
  where the window controls of the skin apply).
- `controls` are the hardware or system buttons the profile offers; the strip in the UI is generated
  from this list and nothing else. `events` is the set of environment events the panel offers for the
  profile (section 4).
- `size`, insets, cutout and fold come from the skin manifest; a profile references a skin by name.
  Bundled profiles only in V1 (the same owner decision as skins): Android phone, iOS phone, Android
  tablet, foldable (book and tabletop), and desktop Windows, macOS and Linux windows.

## 3. Switching device

Selecting a profile applies, in this order: `viewport.set` (size, `dpr`, insets, fold), the profile's
platform record to the viewer, then **restarts the app**: a new session from a cold start, state reset,
the startup pipeline running as on a launch. This is the default because a different device can change
the platform, the available controls and the initial environment, and a launch is what a user of that
device would see. A **Restart app** control does the same without changing device.

Two alternatives are offered as a setting, never silently:

- **Keep state on device change** applies only a configuration change (size, DPR, insets, fold) to the
  running tree in place, the path the engine needs anyway ([#436](https://github.com/thnak/Tessera/issues/436)).
  It is allowed only between profiles of the same `os`; a change of `os` always restarts.
- **Restart** always discards state and the navigation stack.

## 4. Platform events and controls

The UI offers the controls and events of the current profile and turns each into a **normalized
event** delivered through the engine's own path, so the app sees exactly what the HAL would give it.
There is no preview-only handler, the same rule as the fold posture ([designer-studio-v1.md](designer-studio-v1.md)
section 6.1).

| Control or event | Normalized | Runtime effect | State |
|---|---|---|---|
| Back (Android button or gesture, desktop `Esc`/browser back) | `BackRequested` (new input event) | topmost dismissible overlay (`dismiss_on_back`), else `NavigationController::back()`, else **unhandled** | **new** |
| Home | `WindowHidden` then `Suspend` | pause pacing and presenting; app keeps its state | existing events, mapping new |
| Recents | `WindowDeactivated` | focus lost, still running | existing |
| Volume up and down | `VolumeKey` (new input event, `up` or `down`) | delivered to the app; no default action in the emulator | **new** |
| Power | `ScreenOff`, then `ScreenOn` on wake | pause and resume presenting | existing |
| Theme, locale, text scale, orientation, IME show and hide, fold posture | `ThemeChanged`, `LocaleChanged`, `TextScaleChanged`, `OrientationChanged`, `ImeChanged`, `FoldChanged` | style, layout and text as in `os-events-v1.md` section 11 | existing; `viewport.set` already carries some |
| Memory pressure, battery saver, thermal | `MemoryPressure`, `BatterySaverChanged`, `ThermalStateChanged` | evict caches, power policy | existing |
| Kill process and relaunch | none: a session restart with a saved-state record | the app comes back from its saved state, or cold if it saved none | **new model** |

Rules:

1. **Unhandled Back closes the app**, as on Android: the emulator shows an "app closed" state with a
   Relaunch control, and the state is discarded. A handled Back (an overlay or a route popped) leaves the
   app running. Which of the two happened is visible in the event log.
2. **Home then Resume is a hot resume** ([startup-api-v1.md](startup-api-v1.md) section on Hot Resume):
   no startup phase, no rebuild. A relaunch after process death is a cold start, restoring saved state
   if the app declared any.
3. Events are delivered **in `os-events-v1.md` section 3 order** and coalesce by the same rules. The
   emulator adds nothing the HAL would not.
4. **Determinism.** Timestamps come from the simulated clock; the panel keeps an ordered **event log**
   and can export and replay it (`os-events-v1.md` section 12, record/replay scripts), so a bug found by
   hand becomes a repeatable fixture.

## 5. Transport

The studio protocol gains **one** message, `platform.event`, host to viewer, carrying one normalized
event (`type`, optional `args`, the simulated `timestampNs`); the viewer feeds it to the same injector
and dispatch path the native app uses and answers with the effect (`handled`, `appClosed`,
`resumed`). A device switch is `viewport.set` plus `platform.event` records followed by the session
restart. [designer-protocol-v1.md](designer-protocol-v1.md) gets the message, and the sentence in
[designer-studio-v1.md](designer-studio-v1.md) section 4 that says the studio adds no message types is
amended to name it.

## 6. Live edit (hot reload)

Editing source applies automatically; the developer does not press Run.

- **Loop:** edit, about 150 ms debounce, `source.update` (full file text, new version), the viewer
  re-evaluates carrying state by the existing rule (component id, cell id and IR type), one frame
  ([designer-studio-v1.md](designer-studio-v1.md) section 7, [designer.md](../design/designer.md) section 4).
  The engine half exists; this makes it automatic.
- **Run becomes Restart app** (section 3): the explicit cold start. `Ctrl+Enter` keeps meaning "apply now"
  without waiting for the debounce.
- **A failed parse or analysis keeps the last good frame** and shows diagnostics at the span; the preview
  never goes blank while typing.
- **What is kept:** state cells whose keys still match, hover, focus and scroll where the node survives.
  **What resets:** a structural change that removes the node, and a project-file change that is not a
  source file (config, theme), which restarts.
- This is the **browser** live edit. The native app's hot reload is a separate, blocked item
  ([devtools-protocol-v1.md](devtools-protocol-v1.md) section 8) and is not changed here.

**Status: implemented in the browser studio (issue #444).** The browser half of this section is one source
of edits driving all three panes ([designer-studio-v1.md](designer-studio-v1.md) section 1), not one pane at
a time. How it is built, and the surfaces the acceptance is measured on, is
[studio-page.md](../design/studio-page.md) section 1: one debounce timer in `designer/web/src/page-live.ts`,
`apply()` (carrying) distinguished from `run()` (**Restart app**, cold), the canvas module's apply-then-draw
pair rather than a stateless frame (which is what makes a failed parse cost a diagnostic and not the
picture), and `tessera_canvas_apply`'s `changed`/`frames` fields -- the engine's own counters, so "one frame
per burst, zero when static" is read rather than asserted. The two reset cases are detected exactly as this
section states them: a removed node as the carry rule's `droppedComponent`, and a non-source project file
from the edited file's `FILE_KIND`. `scripts/ci/preview_browser_live_edit.mjs` is the developer-run probe for
the five bullets above. The native app's hot reload remains the blocked item named in the last bullet, and
nothing in this slice changes it.

## 7. Not in scope

Emulating the OS or running Android code; sensors (accelerometer, GPS), telephony, notifications and
the camera; vendor device art; user-provided profiles; a second window or picture-in-picture
([designer-studio-v1.md](designer-studio-v1.md) section 6, deferred).

## 8. Slices

| Slice | Work | Acceptance |
|---|---|---|
| E1 (#445) | Engine: `BackRequested` and `VolumeKey` input events, the Home, Recents and Power mappings, unhandled-Back result, saved-state record | unit tests over `ManualClock`: Back pops an overlay, then a route, then reports unhandled; order and coalescing per `os-events-v1.md` section 3 |
| E2 (#446) | Protocol and Wasm: `platform.event`, the viewer path, the effect reply | Node probe delivers each event; parity with the native injector on the same script |
| E3 (#447) | Device profiles as data, selector, restart policy, `os` change always restarts | profile validation, restart resets state, same-`os` keep-state applies a config change only |
| E4 (#448) | Platform panel: control strip, event buttons, ordered log, export and replay | a recorded session replays to the same final state |
| E5 (#444) | Live edit: debounce, Restart replaces Run, last good frame on error | an edit re-renders with state kept and no Run; a syntax error keeps the frame |

E5 does not depend on E1 to E4 and can land first. It has: its browser half is implemented (section 6).

## 9. Working choices (revisable)

The owner accepted the spec without answering these, so the farm proceeds on the first option of each and
the owner can change them later; changing one is an edit to the slice named, not a redesign.

1. **Process death fidelity (E1).** Working choice: the emulator snapshots the state cells by the P4 rule.
   It is cheaper and already exists. The alternative, an app-declared hook in the Android
   `onSaveInstanceState` shape, matches a device more closely.
2. **Gesture navigation (E4).** Working choice: buttons now. Android's back gesture and iOS's home swipe are
   pointer gestures at the screen edge; the gesture itself comes later.
3. **Which bundled profiles (E3).** Still open: tied to the skin list (open question 3 of
   `designer-studio-v1.md`). E3 starts with the list in section 2.
