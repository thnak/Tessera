# ADR-044: Window chrome policy in V1: system by default, Windows client-side title bar opt-in

Status: Proposed (revised 2026-11-11 after owner review; the original text deferred all client-side
decorations, and the owner chose to ship a Windows custom title bar early and to make the app shell
framework-owned).

## Context

The startup surface ([`startup-api-v1.md`](../docs/specs/startup-api-v1.md), ADR-042, ADR-043)
defines the first frame and the loading screen, but not the frame around them. On desktop the
operating system or the window manager draws the title bar, caption buttons and (on macOS) the menu
bar; on mobile the status bar, navigation bar and display cutout are drawn by the system UI. The
pipeline cannot make those pixels consistent by itself, yet it owns the moment they first appear:
the window is shown after the first frame, an Android activity shows the system bars during the
splash, and a title bar or status bar in the wrong theme is immediately visible.

Three pressures are in conflict:

1. **Continuity.** A white title bar or a light status bar around a dark loading screen looks like a
   bug; launch artifacts, loading screen and chrome must derive from one value.
2. **Branding.** Apps want a title bar and an action bar that match the app, not the OS. The owner
   chose that this is worth doing on Windows in V1, and that the app shell should be a
   framework-owned component rather than a library component the app re-implements.
3. **Correctness cost.** A client-side title bar means non-client hit-testing, dragging, resize
   borders, Snap Layouts support, and accessibility for the controls. On Linux/X11 the window
   manager owns decorations and the app must take over resize and move protocols to replace them;
   macOS has its own traffic-light and window-state rules.

The theme asset already defines colours but has no reserved token for the window surface, so the
startup spec's "background token" reference had no normative source.

## Decision

1. **System chrome is the default on every platform.** With no configuration, the OS owns the title
   bar, menu bar and system bars; Tessera paints none of them.
2. **Windows V1 adds an opt-in client-side title bar.** `window.titleBar: "custom"` makes the
   framework draw its own title bar and window controls on Windows, because the owner wants early
   branding and chrome consistency. Linux and macOS stay system-only in V1; requesting `custom`
   there warns (`TESSERA_CHROME_UNSUPPORTED`) and keeps the system chrome, so one project source
   still runs everywhere.
3. **The app shell is framework-owned.** `WindowChrome` (title bar and window controls) and `AppBar`
   (the action row) are framework components imported from `"tessera"`, not library components.
   `AppHost` mounts `WindowChrome` when custom decoration is enabled, already in P2, so no frame is
   ever visible without a title bar. `AppBar` is mounted by the app at P4.
4. **The appearance contract is unchanged and shared.** Two reserved theme tokens
   (`--window-background`, `--window-foreground`) plus `window.appearance` are resolved at build
   time into the startup manifest as `{background, foreground, scheme}`. The P0 launch artifact, the
   P2 default loading screen, the P1 platform call and the engine-drawn custom title bar read that
   one record.
5. **Custom decoration is applied in P1, before `show()`.** `WM_NCCALCSIZE` and the decoration state
   are set on the hidden window, so the first visible frame already has the custom bar. The HAL
   owns the non-client hit-test (`WM_NCHITTEST`): resize borders, `HTCAPTION` drag, `HTSYSMENU`,
   and `HTMAXBUTTON` for Snap Layouts on Windows 11; double-click maximize, the system menu,
   per-monitor DPI and the maximized/restored icon state are part of the contract.
6. **Unsupported chrome degrades, never fails.** An older Windows build that ignores a DWM
   attribute, a platform without the requested decoration mode, or a compositor that forces
   server-side decorations produces a structured warning and default OS chrome.
7. **The custom title bar is accessibility content.** Its window controls are framework nodes with
   names, roles and actions exposed through the platform accessibility bridge; they are not part of
   the app component tree and cannot be styled or hit-tested by app code.

## Alternatives

- **System chrome only (the original proposal).** Cheapest and most native, but the owner wants a
  branded Windows title bar in V1; rejected.
- **Client-side decorations everywhere in V1.** Consistent branding, but Linux resize/move protocols
  and macOS window behaviour are a large, hard-to-reverse surface that no current acceptance
  criterion needs. Rejected for V1; Windows only, with the same contract extended later.
- **Library `AppBar` / app-drawn title bar.** The app would re-implement window controls, drag,
  Snap Layouts and accessibility per project, and the loading screen could not show a consistent
  bar. Rejected; the shell is framework-owned.
- **Per-platform chrome props in JSX.** `titleBarColor`, `statusBarStyle`, etc. per platform leak
  platform decisions into app source and double-maintain every platform. The theme token + scheme
  pair keeps one declaration and one resolved value. Rejected.
- **Let the app draw a fake title bar inside the client area while keeping the system title bar.**
  Double chrome, double close buttons, a title bar that cannot be dragged for move, and a
  hit-testing trap. Rejected.

## Consequences

- V1 gains a Windows-only decoration path: `WM_NCCALCSIZE`, `WM_NCHITTEST` (resize/Snap Layouts),
  DWM frame and shadow handling, window-state icons, high-DPI metrics, and accessibility for the
  window controls. This rides the B3 window work and the startup slice S3, not a new slice.
- The custom bar is engine content, so it must be present in the P2 loading frame. The default
  loading screen renders below it; a custom loading subtree renders inside the content area.
- Title text needs a font. If the app font has not loaded at P2 (critical resource still in
  flight), the bar shows the mark and controls without text and fills the title at P4; no system
  font is ever loaded.
- Linux/macOS custom decoration is deferred; a project that requests it still runs with system
  chrome and one warning, so cross-platform projects are not blocked.
- Two decoration modes must be tested (system and custom) on Windows, including P2 continuity,
  DPI changes, maximize/restore, Snap Layouts and screen readers.
- The studio must preview both modes: system chrome is simulated by the skin, custom chrome is
  drawn by the engine and therefore part of the canvas (the hosted studio design in
  [`designer.md`](../docs/design/designer.md) section 2.2).
- New public contract: `window.titleBar`, the framework `WindowChrome`/`AppBar` components, and the
  two reserved tokens. All are documented normatively in
  [`window-chrome-v1.md`](../docs/specs/window-chrome-v1.md).
