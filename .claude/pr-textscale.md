## Text: wire the off-thread glyph scale step into the window host (#993)

The window host now moves to a new render-scale step without rasterizing the new glyphs on the UI thread (rest of #948).

### What changed
- `TextRenderer::begin_aa_policy_step(policy, executor)`: posts the prewarm for a scale change and keeps the old scale, hinting, AA mode and subpixel rule until `poll_render_scale_step` lands them together (the prewarmed bitmaps are rasterized in the new mode). Scale unchanged: applied directly like `set_aa_policy`. Refused task: returns false and changes nothing. New `cancel_render_scale_step()` for teardown.
- Prewarm keys drop the quarter-pixel fraction when the new step has whole-pixel origins.
- `TextStack` owns a one-worker `WorkerIoExecutor` and its `CompletionQueue` (declared after the renderer so they die first; the destructor cancels the step, then the executor joins: no detached tasks, no `wait_idle`).
- `WindowHost::Impl::apply_aa_policy` goes through the async path (sync fallback only if the executor refuses). `tick_text_scale()` runs each loop iteration in `run()` and `pump_once()`: bounded completion drain plus a flag-read poll; a landed or pending step keeps requesting frames.
- Docs: `docs/design/text-gpu-raster.md` window-host paragraph.

### Verification
- `text_prewarm` gained `policy_step`: ManualIoExecutor, deterministic. Covers pending keeps old policy, landing switches scale and hinting together, glyph equals synchronous result, refused task, step abandoned when returning to the active policy.
- Windows (vs2026 Release): text_*, scale*, app_window*, app_host* all pass (61/61 in the app group). `app_window_offscreen` hung once under `ctest -j8` and passes alone in 1.5 s and in the sequential group; looks like the known GPU-contention flake, not this change.
- `python -B scripts/gates.py --quiet`: 15/15. clang-format 22.1.3 clean.
- Not run: Linux/lavapipe goldens via thnak-ssh (goldens are unchanged by design: the output is byte-identical to the synchronous path).

### Left
- With subpixel positioning on at the new step, only glyphs whose origin fraction is unchanged are prewarmed exactly; others rasterize on demand (same as before this change, no regression).

Closes #993
Refs #948
