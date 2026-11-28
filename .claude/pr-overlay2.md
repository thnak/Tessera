## Overlay follow-ups: non-modal focus, failed-mount fallback (#305)

### What changed
- `OverlaySpec::focus` (`OverlayFocus::{Default, Content, None}`): an overlay-layer entry can take initial focus on its first focusable node after layout, and returns focus to the previous target on close (only if focus was still inside). Tooltips/toasts never take focus.
- Host hook: `AppHost::attach_focus_manager` (wired by `WindowHost`), `OverlayHost::set_focus_manager/apply_focus`.
- Failed mount: `OverlaySpec::fallback` (entry stays open with deterministic content) and `on_failure(handle, message)` error-boundary report, `OverlayHost::last_failure()`. No fallback or a throwing one drops the entry as before.
- Spec: overlays-and-layers-v1 section 10.3; public-api baseline +2 (additive).

### Verification
- ctest overlay_host (2 new tests), overlay_dismiss, window_overlay pass on Windows vs2026-release.
- python -B scripts/gates.py --quiet: 15/15; clang-format 22.1.3 applied.
- Not run: Linux/lavapipe build (no goldens touched; changes are scene/host logic only).

### What is left
Owner-gated remainder moved to #1059: tessera-ui Dialog/Popover/Tooltip/Toast/Menu (compiler lowering + anchor naming decision) and aria-live announcements.

Closes #305
Refs #1059, #1048, #1052

🤖 Generated with [Claude Code](https://claude.com/claude-code)
