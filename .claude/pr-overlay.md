## Window driver draws the diagnostics overlay (#1024)

### What changed
- New runtime config key `diagnosticsOverlay` (default off, documented in runtime-config-v1).
- `src/app/window_overlay.{hpp,cpp}`: `FrameRateMeter` (EMA of the observed frame cost, overrun count), `build_window_overlay_frame` (frame, damage, budget, layers via `fill_layer_overlay`), and `WindowHost::Impl::draw_diagnostics_overlay`, one rounded panel plus text through the window's own text stack, after the scene and before `renderer()->end()`.
- `OverlayFrame::fps` adds an `fps N  frame Mms` line (omitted at 0, so the PoC baseline is unchanged).
- `AppHost::layer_promotion_stats()` accessor.
- Branch is fast-forwarded onto farm-15 because `fill_layer_overlay` (#1014) lives there; merge after that PR.

### Verification
- Windows vs2026 Release: window_overlay, diagnostics_overlay, runtime_config, window_config, app_window_overlay (real window, presents with the overlay on) pass.
- gates.py --quiet 15/15 (versioning baseline updated for the new key); clang-format 22.1.3 clean.

### What is left
- No pixel golden: the window path presents to a swapchain with no readback (capture_frame bypasses it), so the draw is a smoke test and the text is pinned by unit tests. A capture seam on the window path would allow a lavapipe golden.
- Overlay numbers lag one frame; damage rects equal scissors.

Closes #1024
