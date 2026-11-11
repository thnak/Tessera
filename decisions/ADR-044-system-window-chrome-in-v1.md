# ADR-044: System-owned window chrome in V1

Status: Proposed (2026-11-11).

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
2. **Branding.** Apps want a custom title bar (an app-drawn bar with window controls) for a
   uniform look across platforms.
3. **Correctness cost.** A client-side title bar means per-platform non-client hit-testing, dragging,
   resize borders, Snap Layouts support, traffic-light placement, and accessibility for the
   controls. On Linux/X11 the window manager owns decorations and the app must take over resize and
   move protocols to replace them. This is a large, hard-to-reverse surface that no V1 acceptance
   criterion needs.

The theme asset already defines colours but has no reserved token for the window surface, so the
startup spec's "background token" reference had no normative source.

## Decision

1. **System chrome stays system-owned in V1.** Tessera paints no title bar, no window controls and
   no system bar. The client area is the whole scene; on desktop the title bar is outside it and
   desktop insets are zero.
2. **The appearance the framework supplies is a policy, not a widget.** Two reserved theme tokens
   (`--window-background`, `--window-foreground`) plus `window.appearance` (`auto`/`light`/`dark`)
   are resolved at build time into the startup manifest as `{background, foreground, scheme}`. The
   P0 launch artifact, the P2 default loading screen and the P1 platform call read that one record;
   `tessera_codegen --check` treats divergence as a build error.
3. **Chrome is applied in P1, before the window is shown**, and re-applied at a frame boundary when
   the theme, DPI or insets change. It is a bounded HAL call on the window's thread, never a
   compositor round-trip and never a startup gate.
4. **The app action bar is app chrome.** `AppBar` stays a library component mounted at P4; the
   framework synthesizes no action bar, no title text and no menu button. A static header in a
   custom loading subtree is allowed as a continuity guideline.
5. **Unsupported chrome degrades, never fails.** An older Windows build that ignores a DWM
   attribute, or a compositor that forces server-side decorations, produces a structured
   `TESSERA_CHROME_UNSUPPORTED` warning and default OS chrome.
6. **Client-side decorations are deferred.** Adding them later requires its own ADR with a
   per-platform matrix (Windows `WM_NCCALCSIZE`/`WM_NCHITTEST`/Snap Layouts, Linux X11/Wayland
   resize and move obligations, macOS traffic lights) and an accessibility plan for the custom
   controls.

## Alternatives

- **Custom title bar from day one (CSD everywhere).** Best branding, but it forces non-client
  hit-testing, resize borders and window-control accessibility into the first release, risks
  platform breakage (Snap Layouts, Wayland forcing SSD), and would make the startup story depend on
  a component that does not exist. Rejected for V1.
- **Per-platform chrome props in JSX.** `titleBarColor`, `statusBarStyle`, etc. per platform would
  leak platform `ifdef`-style decisions into app source and double-maintain every platform. The
  theme token + scheme pair keeps one declaration and one resolved value. Rejected.
- **No chrome control at all.** Simplest, but leaves the wrong-theme flash the startup spec
  explicitly forbids (G6 continuity) and gives no way to theme dark apps. Rejected.
- **Let the app draw a fake title bar inside the client area while keeping the system title bar.**
  Double chrome, double close buttons, a title bar that cannot be dragged for move, and a
  hit-testing trap. Rejected.
- **Put chrome policy in `tessera.json` only, with no theme tokens.** Chrome would ignore runtime
  theme switching and could drift from the loading screen. Rejected; config owns the switch,
  the theme owns the colours.

## Consequences

- Positive: no chrome flash on either side of the desktop/mobile split; one value drives P0/P1/P2;
  the first release avoids the hardest cross-platform decoration work; desktop layout and goldens
  are unchanged because desktop insets stay zero.
- Negative: V1 apps look native rather than uniformly branded on desktop; the caption colour is
  partly an OS-version lottery (Windows 10 vs 11) and needs the degradation rule; a project that
  wants a branded title bar must wait for the CSD ADR.
- New public contract: two reserved theme tokens and one config key. Both are documented
  normatively in [`window-chrome-v1.md`](../docs/specs/window-chrome-v1.md), and the stylesheet
  theme asset section ([`stylesheet-v1.md`](../docs/specs/stylesheet-v1.md) section 9.2) defines
  their value rules.
- Follow-ups: chrome work rides startup slice S3 (desktop) and S6 (mobile artifacts); no new slice
  is added. The CSD decision (section 12 of the spec, open questions 1-2) stays open.
