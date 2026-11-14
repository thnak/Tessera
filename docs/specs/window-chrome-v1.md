# Window chrome and system bars v1

Status: proposed. Windows is V1 scope including the opt-in `custom` title bar; Linux keeps the
system chrome in V1; the Android entry is Big Step F and the macOS entry is deferred by
[ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md).
Decision: [ADR-044](../../decisions/ADR-044-system-window-chrome-in-v1.md) (system-owned chrome in
V1, no client-side title bar).
Companion to [startup-api-v1.md](startup-api-v1.md) (phases, first frame, loading screen),
[os-events-v1.md](os-events-v1.md) (normalized events, insets, lifecycle),
[responsive.md](../design/responsive.md) (safe areas),
[window-resize.md](../design/window-resize.md) (non-client behavior),
[platform-shell-v1.md](platform-shell-v1.md) (menu model) and
[mobile-integration.md](../design/mobile-integration.md) (mobile host requirements).

The startup pipeline ends up painting into a surface that the operating system has already framed.
This spec owns that frame: how the title bar and menu bar on desktop and the status and navigation
bars on mobile are styled during launch and after it, so the first frame never appears inside
chrome that belongs to a different theme. It is the missing half of the loading-screen contract in
[startup-api-v1.md](startup-api-v1.md) section 6.

## 1. Terms

| Term | Owner | Examples | This spec |
|---|---|---|---|
| System chrome | OS / window manager | Windows title bar and caption buttons, macOS title bar and global menu bar, Android status/navigation bars and display cutout, iOS status bar and home indicator | appearance policy (section 3), timing (section 5) |
| Framework chrome | framework | `WindowChrome` title bar and window controls, `AppBar` action row | engine-drawn, framework-owned (section 8) |
| Window chrome policy | app, resolved by the framework | `--window-background`, `--window-foreground`, `window.appearance`, `window.titleBar` | normative here |
| App chrome | app components | toolbars, bottom navigation, in-page headers | ordinary scene nodes at P4 |

The app shell has two framework-owned pieces (section 8): `WindowChrome`, the title bar and window
controls used when `window.titleBar` is `custom`, and `AppBar`, the action row. Both are engine-drawn
but owned by the framework: app code cannot restyle them node by node, and they are not library
components. Everything else that looks like a bar (toolbars, bottom navigation) is ordinary app
content. A client-side title bar is Windows-only in V1; Linux and macOS keep the system chrome
([ADR-044](../../decisions/ADR-044-system-window-chrome-in-v1.md)).

## 2. Ownership

- System chrome is driven through the HAL/platform backend only
  ([ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md)); UI, components, styles and
  the scene never see a window handle, a DWM/X11/Cocoa/Android call, or a decoration state
  ([AGENTS.md](../../AGENTS.md) non-negotiable 13).
- `AppHost` applies the chrome policy in P1 from the resolved startup manifest
  ([startup-api-v1.md](startup-api-v1.md) section 8); no app code runs for it. In `custom` title-bar
  mode it also installs the decoration mode and the non-client hit-test before `show()`.
- The custom title bar is framework content drawn by the engine, but its OS integration (decoration
  mode, resize/drag hit-test, window state) stays in the HAL/platform backend; the scene never
  receives a `WM_NCHITTEST` result, a window handle or a DWM call.
- Chrome is resolved at build time from the theme asset, so the P0 launch artifact, the P2 loading
  screen and the P1 platform call all use one value and the runtime does not need the style engine
  to paint the first frame.

## 3. Appearance contract

### 3.1 Reserved theme tokens

[stylesheet-v1.md](stylesheet-v1.md) section 9.2 reserves two custom properties:

| Token | Meaning | Built-in `default` value |
|---|---|---|
| `--window-background` | Opaque colour of the window, splash and loading surface, and of any chrome the platform lets the app tint | `#ffffff` |
| `--window-foreground` | Colour rendered on `--window-background`; the pair decides light or dark system-bar glyphs and title-bar text | `#111111` |

- They are ordinary custom properties that may be overridden per theme and switched by `setTheme`,
  but the framework also reads them outside the render tree, so their value must be a plain,
  fully opaque colour literal (named, `#hex`, `rgb()`, `rgba()` with alpha 255, `hsl()`). `var()`,
  `calc()`, percentages and lengths are `TESSERA_CHROME_THEME_VALUE` (build error).
- A project without a theme asset inherits the built-in pair. A theme that defines only one of the
  pair reports `TESSERA_THEME_TOKEN_MISSING` and falls back to the built-in value for the other.
- The "background token" of [startup-api-v1.md](startup-api-v1.md) sections 6.1, 6.2 and guarantee
  G6 is `--window-background`.

### 3.2 Appearance switch

`window.appearance` ([project-config-v1.md](project-config-v1.md) section `window`) selects the
light/dark chrome:

| Value | Behaviour |
|---|---|
| `auto` (default) | Derive from `--window-background` luminance (dark background -> dark chrome), so the bar never contrasts with the surface it frames |
| `light` | Force light chrome and dark glyphs |
| `dark` | Force dark chrome and light glyphs |

`auto` follows the window background, not `prefers-color-scheme`: a project painting a dark theme on
a light OS still gets a dark title bar. Whether `auto` should follow the OS instead is open
(section 12).

### 3.3 Resolution and artifacts

`tessera_codegen` resolves `{ background, foreground, scheme }` from the start theme and
`window.appearance` and writes it into the startup manifest. Every consumer reads that one record:

| Consumer | Uses |
|---|---|
| Android splash theme (P0) | `windowSplashScreenBackground` = background; status/navigation bar appearance from foreground and scheme |
| iOS launch screen (P0) | launch background = background; status bar style from scheme |
| Desktop P1 | window background; title bar dark/light and, where supported, caption/text/border colours |
| Custom title bar (Windows) | background/foreground from the same record; window-control glyphs follow the scheme |
| P2 loading screen | background fill; mark and foreground from the pair |
| Mobile system bars (P1) | bar colour = background; light/dark icons from scheme |

The artifacts are generated and compared by `tessera_codegen --check`, so a hand edit that diverges
from the manifest is a build error instead of a P0/P2 mismatch. A runtime `setTheme` re-resolves the
pair and re-applies it at the next frame boundary; the already-shown P0 artifact keeps the start
theme's values, because a launch artifact cannot change after launch.

## 4. Platform matrix

| Platform | System chrome | V1 policy | Deferred |
|---|---|---|---|
| Windows | DWM non-client title bar and caption buttons; opt-in framework title bar | System by default (DWM dark-mode attribute on Windows 10 2004+/11; caption/text/border colours where supported, Windows 11 22H2+). `window.titleBar: "custom"` draws the framework `WindowChrome` (section 8.1) with engine-drawn controls and HAL hit-testing | App actions inside the title bar; Snap Layouts on Windows 10 (Windows 11 only) |
| Linux / X11 | Window-manager decorations (server-side) | System chrome only in V1: Tessera paints no title bar and cannot tint the WM decoration | Client-side decoration with resize borders, move/resize protocols and menu parity |
| Linux / Wayland | `zxdg_toplevel_decoration_v1` | System chrome only in V1: request server-side decoration; when the compositor forces it, accept | Client-side decoration and its shadow/border obligations |
| macOS (deferred) | Native title bar, traffic lights, OS menu bar | System chrome only: `NSWindow.appearance` from the scheme, a title bar continuous with the window background, menu model installed before activation | Custom title bar/traffic-light ownership; whole macOS backend (ADR-010) |
| Android (Big Step F) | Status bar, navigation bar, display cutout | Splash theme plus edge-to-edge; bar colour from the pair, icon appearance from the scheme; content inset by `statusBars`/`navigationBars`/`displayCutout` | immersive/full-screen and system contrast-enforcement edge cases |
| iOS (deferred) | Status bar, home indicator | Launch background and status bar style from the scheme | whole iOS backend |

The menu bar on macOS is system chrome but its *model* belongs to
[platform-shell-v1.md](platform-shell-v1.md) section 6. This spec fixes only when it is installed:
before the window is shown or the app becomes active, so no default menu flashes.

## 5. Startup phases

| Phase | System chrome | App chrome |
|---|---|---|
| P0 `launch` | Platform artifact only (Android splash theme, iOS launch screen); desktop has no window yet | none |
| P1 `runtime` | Window created hidden; chrome policy applied before `show()`; custom decoration mode and non-client hit-test installed; macOS menu installed | none |
| P2 `first_frame` | Unchanged from P1; the frame includes the framework `WindowChrome` in `custom` mode, otherwise it fills the client area (desktop) or the inset content area (edge-to-edge mobile) | Loading subtree (inside the content area in `custom` mode); no interactive chrome |
| P3 `background_init` | Unchanged | Loading subtree |
| P4 `interactive` | Re-applied only if the theme, DPI or insets changed during P3 | Initial UI, including the app action bar |
| P5 `optimizing` | Unchanged | Unchanged |
| `hot_resume` | Re-applied at the next frame boundary only if a theme, DPI or safe-area change happened while away | Unchanged |

On desktop the window is shown after the P2 frame ([startup-api-v1.md](startup-api-v1.md) section
6.2), so the OS default light title bar is never visible around an empty dark window. On mobile the
system bars are visible from P0, which is why the splash theme and the P1 call must agree (section
3.3).

## 6. Insets and coordinates

- Scene coordinates are client-area logical pixels and `WindowDesc.width/height` is the client size
  ([window.hpp](../../include/tessera/hal/window.hpp)). Chrome never changes the scene's coordinate
  space.
- The only inset input is `hal::WindowMetrics::insets` in logical pixels. The HAL normalizes platform
  insets ([os-events-v1.md](os-events-v1.md)) and UI code never branches on the platform.
- Desktop V1 reports zero insets: the title bar is outside the client area and the taskbar/panel is
  not part of the window. Desktop layout therefore stays byte-identical to today.
- Mobile edge-to-edge: the window equals the display; `statusBars`, `navigationBars`,
  `displayCutout` and `ime` insets are published before the P2 layout. The loading screen paints
  edge-to-edge with `--window-background` and places its mark in the inset content rect; app content
  stays inside the safe area unless a node opts into edge-to-edge
  ([responsive.md](../design/responsive.md) section 4).
- If insets are not published yet at P2 they are zero; nothing blocks waiting for them. The default
  loading screen is centered and background-only, so zero insets are visually safe. A custom loading
  subtree must not depend on insets before P2.

## 7. Guarantees

- **C1 — No chrome flash.** The chrome policy is applied before the window is first shown (desktop)
  or before the first frame after the activity starts (mobile). No visible frame has system chrome
  styled from a different background than the frame itself, and in `custom` mode the P2 frame
  already contains the framework title bar (no frameless flash).
- **C2 — One source.** P0 artifact, P2 loading screen, P1 chrome and the custom title bar read one
  resolved record (section 3.3); divergence is a build error, not a visual bug.
- **C3 — Ownership is explicit.** System chrome is OS-owned: no node, style, event target, focus stop
  or accessibility element is created for it, and a caption drag or status-bar pull never reaches
  the scene. Framework chrome is the opposite: `WindowChrome`/`AppBar` are engine-drawn framework
  nodes with framework hit-testing, not app scene content; app code cannot restyle them, and their
  window controls are exposed as framework accessibility nodes.
- **C4 — Insets before first paint.** P2 layout uses the insets published with the frame; a missing
  value is zero, never a blocking query.
- **C5 — Shell is P2/P4, actions are P4.** In `custom` mode the framework mounts `WindowChrome` in
  P2, so the first frame has a title bar; `AppBar` is mounted by the app at P4. The framework
  synthesizes no app action bar, menu button or app title inside the title bar,
  and `loading.screen: shell` keeps falling back to the default screen
  ([startup-api-v1.md](startup-api-v1.md) section 7.2). A custom loading subtree may draw a static
  header inside the content area, never interactive chrome.
- **C6 — No double bar.** Exactly one title bar is visible: the OS's or the framework's, never both.
  The loading subtree must not fake a system bar or a title bar, and app content must not draw a
  second one.
- **C7 — Degrade, never fail.** An unsupported appearance attribute (older Windows build, a
  compositor that forces decorations) is ignored once with `TESSERA_CHROME_UNSUPPORTED`; chrome
  never delays, retries or fails startup.
- **C8 — No blocking.** Applying chrome is a bounded call on the window's thread in P1 before
  `show()`, with no compositor round-trip, wait or frame-path allocation
  ([ADR-021](../../decisions/ADR-021-async-runtime.md)).
- **C9 — Observable.** One `chrome.apply` trace span per launch (and per re-apply) carrying the
  platform, scheme and result; `chromeApplyMs` is the only new metric
  ([diagnostics-config-v1.md](diagnostics-config-v1.md)).

## 8. Framework shell: window chrome and action bar

The shell is framework-owned. `WindowChrome` and `AppBar` are built-in components imported from
`"tessera"`; they are not library components and are not startup declaration elements. App code
cannot restyle or hit-test them node by node, because window controls and window behaviour must stay
correct on every platform.

### 8.1 `WindowChrome` (custom title bar, Windows V1)

Enabled by `window.titleBar: "custom"` ([project-config-v1.md](project-config-v1.md) section
`window`). Windows only in V1; on Linux and macOS the request warns (`TESSERA_CHROME_UNSUPPORTED`)
and the system chrome stays.

- **Ownership and timing.** `AppHost` mounts it in P1 and it is drawn in P2, so the first visible
  frame is never frameless. It occupies the top `chromeHeight` logical pixels of the client area,
  and the app content area starts below it.
- **Content.** App title text (`window.title`), the window-control cluster (minimise, maximise or
  restore, close) and the window/system menu. App actions inside the title bar are not V1
  (question 2).
- **Metrics.** Title bar height is 32 logical pixels at 100% scale, scaled by the per-monitor DPI
  scale; caption buttons follow the platform metrics (Windows 11: 46 x 32 at 96 DPI). The height is
  a framework constant in V1; a theme metric token is open (question 1).
- **Hit-test contract (HAL).** The platform backend answers `WM_NCCALCSIZE` with an empty non-client
  area, then resolves `WM_NCHITTEST` before app routing: edges and corners to the `HT*` resize
  codes, the title area to `HTCAPTION` (drag, double-click maximize, drag-to-snap), the system-menu
  area to `HTSYSMENU`, the maximise button to `HTMAXBUTTON` on Windows 11 (Snap Layouts flyout), and
  every other point to the framework controls or the client area. The framework publishes the
  interactive regions through `hal::Window::set_caption_regions` as logical-pixel
  `hal::CaptionRegion` values (`bounds` plus a `CaptionRegionKind` of drag, system menu, minimise,
  maximise or close), never as `HT*` codes, and the window reports the same list back in
  `WindowMetrics::caption_regions`; an empty list is the system-chrome default where every point is
  client area ([`caption.hpp`](../../include/tessera/hal/caption.hpp)). `WM_NCLBUTTONDOWN`,
  `WM_NCRBUTTONDOWN` (system menu) and the non-client activation path are handled by the HAL, which
  reports only normalized results to the engine.
- **DWM.** Frame and shadow use the platform's documented calls
  (`DwmExtendFrameIntoClientArea`/`DWMWA_*`); rounded corners follow the OS; a translucent window and its backdrop material are specified in
  [`glass-v1.md`](glass-v1.md) section 6; an
  attribute an older build does not support is ignored with one warning (section 9).
- **Window state.** Maximise/restore state, including the button glyph and tooltip, comes from
  normalized window events ([os-events-v1.md](os-events-v1.md)); the HAL owns edge cases such as
  snapping and auto-hide taskbars.
- **Text and fonts.** The title is drawn with the active theme's UI font. If that font is not
  available at P2 (a `critical` font still loading), the bar shows the mark and controls without
  text and fills the title at P4; no system font is loaded for chrome.
- **Accessibility.** The three window controls are framework nodes with roles, names ("Minimise",
  "Maximise"/"Restore", "Close") and actions exposed through the platform bridge
  ([accessibility-v1.md](accessibility-v1.md)); the title is the window name. They are keyboard
  reachable and are not part of the app component tree or its focus order.
- **Keyboard.** No new shortcuts: `Alt+F4`, `Alt+Space`, `F10`/system menu and the OS window
  shortcuts keep working through the platform path. The framework does not intercept app shortcuts
  in the title bar.
- **Failure.** If the decoration mode cannot be installed, the platform falls back to the system
  title bar with `TESSERA_CHROME_UNSUPPORTED`; startup does not fail or retry.

### 8.2 `AppBar` (action row)

- Framework component, not a library component; imported from `"tessera"` and mounted by the app at
  P4. This spec adds no startup element.
- It shows app-level actions, a title and an optional back affordance; back is a platform gesture or
  button on Android and an app action elsewhere. Window controls stay in `WindowChrome`.
- It reads the top safe-area inset through the layout environment
  ([responsive.md](../design/responsive.md) section 4) and, in `custom` mode, sits immediately
  below the title bar rather than consuming an inset.
- The framework never mounts it automatically, so an app that wants no action bar has none.

### 8.3 Continuity

A custom loading subtree renders inside the content area (below the framework title bar in `custom`
mode). A static header in that subtree is allowed, but it must match the P4 `AppBar` height,
background and non-interactive content and must install no focus or hit-test targets. Continuity is
a guideline, not a compiler check.

### 8.4 Full-screen (Windows and Linux)

Full-screen is a V1 shell capability on Windows and X11 (owner decision 2026-11-13, issue #232);
the shell model itself is [platform-shell-v1.md](platform-shell-v1.md) section 6.1.

- **Operation.** The app calls `app::WindowHost::set_fullscreen(bool)` (or `hal::Window::set_fullscreen`
  in backend code) and reads the state back through `WindowHost::fullscreen()`, which mirrors
  `WindowMetrics::fullscreen`. That flag is the resolved mode of the window manager, not the last
  request: a platform that refuses or has not answered yet keeps reporting the old value.
  `hal::Window::supports_fullscreen()` tells a caller whether the platform can do it at all; false
  does not promise that a request succeeds.
- **Bounds contract.** The platform remembers the pre-full-screen outer bounds (position, size and
  maximized/restored state) and restores them on exit; the app never caches or reapplies a
  rectangle, and no bounds are cached in the app layer. A repeated request (enter while
  full-screen, exit while windowed) is a no-op, so a stale restore cannot overwrite a resize the
  user made in between. The bookkeeping is the platform-neutral `hal::FullscreenState`
  ([`fullscreen.hpp`](../../include/tessera/hal/fullscreen.hpp)), shared by both backends:
  `fullscreen_enter` records the bounds once, `fullscreen_exit` consumes the record,
  `fullscreen_resolved` folds a mode the user changed outside the app.
- **Windows.** A borderless `WS_POPUP` window covering the monitor the window is on; the style and
  `WINDOWPLACEMENT` are saved on entry and reapplied with `SetWindowPlacement` on exit, so position,
  size and the maximized/restored state return exactly. The resolved flag compares the outer rect
  with the monitor rect, so a maximized window is not full-screen.
- **Linux / X11.** EWMH `_NET_WM_STATE` ClientMessage adding or removing
  `_NET_WM_STATE_FULLSCREEN` (source indication 2). Support requires a window manager advertising
  `_NET_WM_STATE` in `_NET_SUPPORTED`; a bare Xvfb has none and is unsupported. The mode is read
  back from `_NET_WM_STATE` every time metrics are built, so a window-manager shortcut or a taskbar
  toggle is observed. The client rectangle is remembered by the backend and restored only when no
  window manager restored it.
- **Deferred.** Wayland full-screen is deferred with the Wayland window backend (#155); macOS,
  Android and iOS are post-V1. X11 has no client-side decoration, so `set_caption_regions` stores and
  reports the list but no X protocol hit test consumes it.
- **Escape and shortcuts.** Escape and the platform shortcuts are the app's and the window manager's,
  not the framework's ([media-components-v1.md](media-components-v1.md) section 8); a mode change
  made outside the app is observed through the metrics, never as a separate event.
- **Diagnostics.** One `TESSERA_WINDOW_UNSUPPORTED` warning per window on the first request where the
  platform cannot do full-screen (sections 9 and 10); the window keeps its windowed mode and startup
  does not fail or retry.
- **Tests.** `window_fullscreen` (state machine and caption resolver, display-free),
  `window_fullscreen_fake` (fake-window round trip, refusal path, metrics dump) and `window_win32`
  (real Win32 enter/exit restores the client size).

### 8.5 Cursor shape (Windows and Linux)

The CSS `cursor` property reaches the native pointer on Windows and X11 (owner decision 2026-11-14,
issue #191). This subsection is the normative home of the V1 cursor grammar and the HAL contract;
`stylesheet-v1.md` section 7.3 lists `cursor` with the other inherited properties.

- **Value grammar.** One keyword from the CSS cursor set: `auto`, `default`, `none`,
  `context-menu`, `help`, `pointer`, `progress`, `wait`, `cell`, `crosshair`, `text`,
  `vertical-text`, `alias`, `copy`, `move`, `no-drop`, `not-allowed`, `grab`, `grabbing`,
  `all-scroll`, `zoom-in`, `zoom-out`, `n-resize`, `e-resize`, `s-resize`, `w-resize`,
  `ne-resize`, `nw-resize`, `se-resize`, `sw-resize`, `ns-resize`, `ew-resize`, `nesw-resize`,
  `nwse-resize`, `col-resize`, `row-resize`. Matching is case-insensitive and trimmed.
  `auto` is the initial value and resolves to the platform default arrow (never "whatever the OS
  thinks", so a result is deterministic); `default` is the same arrow. `none` hides the pointer.
  An unknown keyword falls back to `auto`. A `url(...)` cursor list (and the comma list around it)
  is **not** implemented in V1: it produces one `StyleDiagnosticKind::UnsupportedValue` per
  declaration, the declaration stays in the AST, and the computed value is the initial `auto`.
  CSS-wide keywords and `var()` are the cascade's business, not a value-form diagnostic.
- **Layers.** `ui/style` parses the keyword into `style::CursorKind` (`cursor_value.hpp`; one
  grammar for the declaration validator, `@supports` and the sync), the scene stores the resolved
  kind on the node (`Node::cursor`, inherited, so the computed value already carries the ancestor
  resolution), the app maps it onto `hal::CursorShape`, and the platform maps that onto the native
  cursor. `ui/` never includes a `hal::` header (ADR-020).
- **HAL contract.** `hal::Window::set_cursor(hal::CursorShape)` requests a shape and
  `supports_cursor()` says whether the platform can do it at all; `WindowMetrics::cursor` reports
  the shape the platform **resolved**, like `fullscreen` reports the resolved mode. `CursorShape`
  is the app-visible intent and the platform decides its OS call. The call never blocks, throws or
  fails: X11 applies it with `XDefineCursor` (an Xlib-buffered request, not a round trip) while
  Win32 records it and installs it from the next `WM_SETCURSOR` (the OS re-asserts the cursor after
  every non-client transition, so the class cursor would otherwise snap back).
- **Change-only requests.** The app reads the deepest hovered node's shape after input each frame
  and calls `set_cursor` only when it differs from the last requested shape. A static hover makes
  no platform call, so the path costs no per-frame syscall. Nothing re-applies the cursor on pointer
  entry: the platform keeps the installed shape, and the app re-requests on every change.
- **Hiding.** The pointer is never hidden unless `cursor: none` is in effect: `Hidden` is a
  distinct `CursorShape` that a platform maps to a null cursor, and a platform that cannot hide the
  pointer keeps `Default`.
- **Diagnostics.** One `TESSERA_WINDOW_CURSOR_UNSUPPORTED` warning per window on the first request
  whose shape actually changed on a platform where `supports_cursor()` is false (sections 9 and
  10); the pointer keeps its shape and startup does not fail or retry.
- **Tests.** `window_cursor` (the grammar, case/whitespace, the `auto` fallback, the `url()`
  diagnostic, `@supports`), `window_cursor_fake` (fake-window round trip, refusal path, the
  change-only request and the one-shot diagnostic, the metrics dump) and `window_win32` /
  `window_x11` (a real window reports the shape it resolved).

## 9. Failure and degradation

| Situation | Behaviour |
|---|---|
| `window.appearance` has an unknown value | Config validation error (project-config rule 22); build fails |
| Reserved token is not a plain opaque colour | `TESSERA_CHROME_THEME_VALUE` (build error) |
| Only one token of the pair is defined | `TESSERA_THEME_TOKEN_MISSING` (warning); the other uses the built-in value |
| Platform cannot tint chrome | `TESSERA_CHROME_UNSUPPORTED` (warning, once per platform and attribute); OS default chrome stays |
| `window.titleBar: "custom"` on Linux/macOS | `TESSERA_CHROME_UNSUPPORTED` (warning); system chrome stays, one project source still runs |
| Custom decoration cannot be installed | Platform falls back to the system title bar with `TESSERA_CHROME_UNSUPPORTED`; no startup failure, no retry |
| No app font at P2 in `custom` mode | Title text is blank until P4; the bar, mark and window controls are still drawn |
| Insets not published at P2 | Zero; no wait and no retry |
| Full-screen requested where the platform cannot do it (no backend, no EWMH window manager) | `TESSERA_WINDOW_UNSUPPORTED` (warning, once per window); the window keeps its windowed mode |
| Cursor shape requested where the platform cannot do it (`supports_cursor()` is false) | `TESSERA_WINDOW_CURSOR_UNSUPPORTED` (warning, once per window); the pointer keeps its shape and the app keeps running |
| Headless/null HAL | Chrome application is a no-op with `chrome.result=skipped`; output stays deterministic |
| Window creation fails | Startup fails before P2 as today (startup G7); chrome is not the cause and adds no retry |

## 10. Diagnostics and metrics

| Code | Severity | Meaning |
|---|---|---|
| `TESSERA_CHROME_THEME_VALUE` | error (build) | A reserved chrome token is not a plain opaque colour |
| `TESSERA_CHROME_UNSUPPORTED` | warning (runtime) | The platform or OS version ignored a chrome attribute |
| `TESSERA_WINDOW_UNSUPPORTED` | warning (runtime) | The platform cannot do full-screen; the first request per window reports it and the window stays windowed |
| `TESSERA_WINDOW_CURSOR_UNSUPPORTED` | warning (runtime) | The platform has no pointer-shape source; the first changed request per window reports it and the pointer keeps its shape |
| `chrome.apply` | span | One span per application/re-application with platform, scheme, kind and result |

- Metric: `chromeApplyMs` (the P1 application only; re-applications are counted, not timed
  separately). Labels: `chrome.platform` (`win32`, `x11`, `wayland`, `macos`, `android`, `ios`,
  `null`), `chrome.scheme` (`light`, `dark`), `chrome.kind` (`system`, `custom`) and
  `chrome.result` (`applied`, `partial`, `skipped`).
- Disabled diagnostics add no measurable cost, and the span is not created when tracing is off
  ([ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md)).

## 11. Tests

- Unit: resolution of `{background, foreground, scheme}` from the built-in theme, a project theme
  and a theme override; `auto` luminance threshold; light/dark overrides; single-token fallback;
  rejection of `var()`/`calc()`/translucent values.
- Deterministic headless: chrome application on the null HAL is a no-op with `chrome.result=skipped`
  and byte-identical frames; the trace contains exactly one `chrome.apply` span.
- Golden: default loading screen at two window sizes, two DPI scales, both schemes and reduced
  motion; desktop client-area frame unchanged while the window is hidden before P2.
- Windowed POC (Windows/Linux): the window is created hidden, chrome is requested before `show()`,
  and the first visible frame after `show()` is the P2 frame.
- Platform matrix: a fake backend records the exact chrome request (scheme, colours, title) per
  platform and asserts the unsupported paths report `TESSERA_CHROME_UNSUPPORTED` once.
- Mobile (deferred backend): splash theme and P2 frame share one background; system bars never
  change colour between P0 and P2; loading content stays inside the insets.
- Artifact: `tessera_codegen --check` fails when a generated launch artifact diverges from the
  manifest pair.
- Custom title bar (Windows, windowed tests): the P2 frame contains the bar; the app content area
  starts below it; `WM_NCHITTEST` routes edge resize, caption drag, double-click maximize, the
  system menu and the window controls; the maximise glyph follows window state; Snap Layouts sees
  `HTMAXBUTTON` on Windows 11; a DPI change re-measures the bar; the controls expose names/roles
  through UIA; requesting `custom` on another platform warns and keeps system chrome.
- Studio preview: `custom` mode renders the bar through the engine (not through the skin), and the
  simulated system chrome is hidden (see the hosted studio design in
  [designer.md](../design/designer.md) section 2.2).
- Full-screen and caption regions (Windows/Linux): `window_fullscreen` covers the display-free
  state machine (enter records bounds once, repeated enter is a no-op, exit consumes the record, a
  mode changed outside the app is folded in, stale bounds dropped) and the caption resolver (region
  order, half-open right/bottom edge, non-finite point is client area, `is_caption_button`);
  `window_fullscreen_fake` drives the fake window round trip, the refusal path and the
  `format_window_metrics` dump (`fullscreen 0|1`, `caption_regions N`, one `caption <kind>` line per
  region); `window_win32` asserts a real Win32 enter/exit restores the client size and that caption
  regions publish and clear.
- Cursor shape (Windows/Linux): `window_cursor` covers the display-free grammar (every supported
  keyword parses to its kind, case/whitespace insensitivity, an unknown keyword falls back to
  `auto`, a `url()` cursor list and the comma list around it report exactly one `UnsupportedValue`
  and keep the declaration, `@supports` agrees with the validator) and `interaction_style`
  covers the node propagation (`cursor: pointer` reaches `Node::cursor()`, it is inherited, a later
  value overrides, `changed_cursor` counts, a steady second sync reports no change);
  `window_cursor_fake` drives the fake-window round trip, the refusal path, the app's change-only
  request and its one-shot diagnostic, and the `format_window_metrics` `cursor <shape>` line;
  `window_win32`/`window_x11` assert a real window reports the shape it resolved.

### 11.1 Manual checklist: translucent window and OS backdrop

The real DWM and compositor calls of [glass-v1.md](glass-v1.md) section 6 cannot be asserted in CI
(no Mica in a VM, no compositor on Xvfb): the table below is the manual evidence for a change that
touches the window path. Record the OS build, the compositor and version, the requested
`window-backdrop`, and the resolved `WindowMetrics::backdrop` for each run.

| # | Platform | Steps | Expected |
|---|---|---|---|
| 1 | Windows 11 22H2+ | `backdrop: mica`, `transparent: true` | `backdrop=mica`, the desktop material is visible through the client area, no warning |
| 2 | Windows 11 22H2+ | `backdrop: acrylic` and `backdrop: blur` | `backdrop=acrylic` for both, no warning |
| 3 | Windows 10 22H2 | `backdrop: auto` | `backdrop=none`, translucent without a material, no warning; the `@media (window-backdrop: none)` rule paints the opaque fallback |
| 4 | Windows 10 22H2 | `backdrop: acrylic` with and without `win10Acrylic: true` | warning and opaque fallback without the opt-in; `backdrop=acrylic` with it |
| 5 | Linux, X11 + KWin (compositing on) | `backdrop: blur`, `transparent: true` | `backdrop=blur`; moving the window blurs the desktop behind it; `_KDE_NET_WM_BLUR_BEHIND_REGION` is set to the client rect (`xprop`) |
| 6 | Linux, X11 + another WM, or no compositing manager | `backdrop: auto` | `backdrop=none`, window opens opaque, no warning for `auto` |
| 7 | Linux, Wayland + KWin | `backdrop: blur` | `backdrop=blur` through `ext-background-effect-v1` once the Wayland window backend (#155) lands; until then the X11/XWayland path reports the X11 row |
| 8 | Any platform | `backdrop: mica` where Mica is unavailable | exactly one `TESSERA_GLASS_WINDOW_UNSUPPORTED` line, startup and rendering continue |
| 9 | Any platform | A swapchain without a premultiplied mode | one `TESSERA_GLASS_WINDOW_UNSUPPORTED` line, the window presents opaque |
| 10 | Any platform | Static translucent screen | the window settles and presents zero frames (no backdrop re-apply or poll on the frame path) |

## 12. Design-time preview

The studio previews the chrome without owning it: the page paints the simulated title bar, menu bar
and status/navigation bars as HTML around or over the engine canvas and hands the skin's insets to
the viewer, so the engine renders exactly the client area (desktop) or the edge-to-edge surface
(mobile) that production would render ([designer-studio-v1.md](designer-studio-v1.md) section 5).
The simulated frame is tinted from the same resolved `{background, foreground, scheme}` record, so a
theme change retints the frame and the canvas in the same frame. The studio never renders system
chrome inside the engine, and a skin's art is geometry only.

## 13. Deferred and open questions

Owner decisions recorded 2026-11-11: `auto` follows the window background; the Windows custom title
bar ships in V1 (`window.titleBar: "custom"`); the app shell is framework-owned; insets reach styles
only through the layout environment.

Deferred (not V1; an addition needs an ADR or a spec revision):

- Client-side decorations on Linux (X11/Wayland) and macOS, including resize/move protocols, traffic
  lights and Wayland decoration negotiation.
- App actions inside the title bar, a command palette, or tabs in the title bar; V1 has the window
  controls and the system menu only.
- A theme token for the title bar height; V1 uses the framework constant.
- Immersive and hidden-status-bar modes; taskbar/dock integration; tray
  ([platform-shell-v1.md](platform-shell-v1.md) owns the model).
- Per-window themes and multiple windows with different chrome.
- Chrome title text from JSX; V1 title text is `window.title`.
- macOS, Android and iOS backends themselves (ADR-010, Big Step F).

Open questions (no decision yet):

1. Title bar height: keep the 32 logical-pixel framework constant, or expose a theme metric token
   (`--window-chrome-height`) with a minimum?
2. Windows 10 and 11 validation matrix: Snap Layouts exist on 11 only; should V1 declare Windows 10
   custom decoration best-effort or fully supported?
3. When the critical font is absent at P2 the custom title is blank. Is a small bundled framework
   title font (OFL) worth the size to avoid it, or is blank-then-fill at P4 acceptable?
4. Accessibility ownership of the custom window controls: framework UIA/AT-SPI nodes separate from
   the app tree, or ordinary scene nodes with reserved roles?
5. Linux/macOS client-side decoration timeline, and whether Linux starts with Wayland or X11.
6. How aggressively to tint the Windows caption in the range of OS builds that support only the
   dark-mode attribute but not the caption-colour attributes; partial schemes may look mismatched.
7. Android system-bar contrast enforcement and gesture-navigation bar transparency vary by OEM;
   whether V1 normalizes them or accepts device defaults.
8. Whether `chromeApplyMs` should have a budget (it is expected to be well under 1 ms) and whether a
   miss is a warning like the startup budgets.
