# Animation runtime

Status: implemented B2 runtime slice. Normative behavior lives in
[`animation-v1.md`](../specs/animation-v1.md); this document explains how the C++ runtime is
assembled, how the pieces interact, and which ordering guarantees callers can rely on.

## 1. Scope

The runtime covers declarative animation that is sampled by the fixed-step loop:

- CSS transitions and `@keyframes` animations, parsed by the style/animation modules.
- Numeric transitions and animated values that use the same `AnimationRuntime`.
- Springs through `SpringRuntime`.
- Per-target style overlays and dirty-domain reporting.
- Wake integration with `RuntimeLoop`.
- Reduced-motion policy.

It does not read wall-clock time. Every advance is an explicit `tick(dt)` with a finite,
non-negative `dt`.

## 2. Module map

| File | Responsibility |
|---|---|
| `ui/animation/keyframes.hpp` | Sample a `KeyframeBlock` at a given elapsed time, including direction, iterations, delay, and fill. |
| `ui/animation/easing.hpp` | Linear, preset, cubic-bezier, and steps evaluation. |
| `ui/animation/runtime.hpp` | Active entries, retained fills, pause/resume/stop/restart, completion callbacks, samples, wake state, dirty domains, reduced-motion timing. |
| `ui/animation/animation_start.hpp` | Resolve keyframes by name and start parsed `AnimationSpec` values. |
| `ui/animation/animation_style.hpp` | Parse computed style, start/ensure/sync style animations, apply play state. |
| `ui/animation/transition_runtime.hpp` | Start matched transitions, diff computed styles, supersede target/property entries. |
| `ui/animation/spring.hpp` | Deterministic semi-implicit Euler spring with retarget and epsilon settle. |
| `ui/animation/spring_runtime.hpp` | Turn spring state into property samples; snap under reduced motion. |
| `ui/animation/style_overlay.hpp` | Apply samples and tick results onto per-target `ComputedStyle`. |
| `ui/animation/property_dirty.hpp` | Map declarations to style/layout/paint/render dirty domains. |
| `ui/animation/loop_bridge.hpp` | Tick the runtime and request `RuntimeLoop` frames only when work remains. |

## 3. Data model

```cpp
struct AnimationInput {
    std::uint64_t target;
    style::KeyframeBlock keyframes;
    KeyframeTiming timing;
    AnimationCallback on_complete;
    std::string property;
    bool paused;
};

struct AnimationEntry {
    AnimationId id;
    std::uint64_t target;
    style::KeyframeBlock keyframes;
    KeyframeTiming timing;
    double elapsed;
    AnimationCallback on_complete;
    std::string property;
    bool paused;
};
```

An entry lives in exactly one of two collections:

- `entries_` — active animations. Paused entries are still active.
- `fills_` — retained `forwards`/`both` final samples after completion.

`active()` counts `entries_`; `settled()` counts `fills_`; `empty()` means no active entries.
A runtime can therefore be "empty" while still holding settled style state.

Ids start at `1` and are never reused. `0` is always invalid.

## 4. Starting an animation

`AnimationRuntime::start(AnimationInput)`:

1. Applies reduced-motion timing if enabled.
2. Allocates a fresh id.
3. Removes an existing retained fill for the same target when either the new keyframes name
   matches or the new non-empty property matches.
4. Appends the new active entry.

Starting the same target/name replaces the old fill instead of stacking two final states.
Starting a different name or property leaves unrelated fills intact.

`start_animations`/`start_style_animations` resolve keyframes first. `none`, empty names, missing
blocks, and non-positive iteration counts produce no entry.

## 5. Tick ordering

`AnimationRuntime::tick(dt)` has a deliberate order:

1. Reject non-finite or negative `dt`. The runtime reports `keep_awake` if any unpaused entry
   exists and changes no state.
2. Advance every unpaused entry by `dt`.
3. Move entries that reached their end into the finished set.
4. For `forwards`/`both`, clamp the elapsed time to the final sample and retain the entry in
   `fills_`.
5. Remove finished entries from the active list.
6. Invoke completion callbacks in finished-entry order, with the original ids.
7. Sample every entry still active, then every retained fill.
8. Accumulate dirty domains from those samples.
9. Set `keep_awake` from the post-callback active entries.

The post-callback phase is important. A callback may clear, stop, restart, or start animations,
and the tick result must describe the runtime after those changes:

- `clear()` in a callback produces no samples and no dirty domains.
- Stopping a sibling drops that sibling's sample.
- Restarting a finished fill produces a zero-time sample, not the old final sample.
- Starting a new animation produces its zero-time sample.

Callbacks never run for entries removed by `stop`/`clear` without finishing.

## 6. Pausing and resuming

- `pause(id)` holds the entry where it is; `resume(id)` continues from that elapsed time.
- Target and target/name variants apply the same operation to matching active entries and return
  the number of matches, including already-paused or already-running entries.
- Empty or case-mismatched names match nothing.
- Retained fills are ignored by pause/resume.
- Paused entries still emit their held sample on each tick for stable overlay rendering, but do not
  advance, finish, fire callbacks, or keep the loop awake.
- A runtime with only paused entries sleeps.

## 7. Retained fills

`forwards` and `both` fills are replayed every tick:

- They contribute samples and dirty domains.
- They do not wake the loop and do not repeat completion callbacks.
- `target_name_state(target, name)` counts them in `settled`, not `active`.
- Starting a replacement for the same target/name or target/property removes the old fill.
- `stop`, `stop_target`, `stop_target_name`, `stop_target_property`, and `clear` remove fills.
- `restart_target_name` and `restart(id)` revive a fill from time zero, preserving its id,
  property, and callback.

## 8. Stopping and restarting

| Operation | Active entries | Retained fills | Callbacks |
|---|---|---|---|
| `stop(id)` | Removes the id | Removes the id | Not fired |
| `stop_target(target)` | Removes all for target | Removes all for target | Not fired |
| `stop_target_name(target, name)` | Exact target/name | Exact target/name | Not fired |
| `stop_target_property(target, property)` | Exact target/property | Exact target/property | Not fired |
| `clear()` | All | All | Not fired |
| `restart_target_name(target, name)` | Reset and unpause | Revive at zero | Later finish fires again |
| `restart(id)` | Reset and unpause | Revive at zero | Later finish fires again |

Name matching is exact and case-sensitive. Unknown targets, names, properties, ids, and `id == 0`
leave state unchanged.

## 9. Style reconciliation

`parse_style_animations` reads longhands first and falls back to the `animation` shorthand. It keeps
longhand values authoritative when both forms are present.

`start_style_animations` starts parsed specs. `apply_style_play_state` changes only matching
active names and returns the number changed. `ensure_style_animations` is idempotent:

- Reuse an existing active target/name entry and apply play state.
- Skip a name that exists only as a retained fill.
- Start only missing names.
- Return ids in spec order.

`sync_style_animations` adds removal:

- Parse the computed style.
- Stop active and retained-fill names for the target that are absent from the desired list.
- Ensure the desired names.
- Return `ok`, `started`, `stopped`, and active ids.

`animation-name: none` means an empty desired list and stops every named entry for that target.
Parse errors return `ok == false` and mutate nothing. Entries with empty keyframe names belong to
property-only transitions and are not removed by name sync. Other targets are untouched.

## 10. RuntimeLoop bridge

`tick_animations(runtime, loop, dt)` ticks the runtime and requests a frame when either:

- `result.keep_awake` is true, or
- at least one animation finished on this tick.

Paused-only, settled-fill-only, and idle ticks request nothing. A finishing tick requests one final
frame so the last sample can be rendered. The bridge returns the full `AnimationTickResult`; callers
still decide how to apply samples and overlay styles.

`ui/frame_pipeline.hpp` (`tessera_frame`) composes the bridge with dirty collection and damage:

- `tick(root, dt)` ticks through the bridge, marks each sampled target with the dirty domains of its
  declarations, collects the dirty pass, and reports `needs_style`/`needs_layout`.
- The caller resolves style/layout; those stages mark paint/render dirty.
- `finish_frame(root)` re-collects paint/render dirty, updates `DamageTracker`, and returns a
  normalized `RedrawPlan`. An empty plan means no frame; area/count overflow is normalized to one
  full-target scissor.

The pipeline clears the damage accumulator at `tick`, so extra damage must be added after `tick` and
before `finish_frame`.

## 11. Reduced motion

`AnimationRuntime::set_reduced_motion(true)` applies the OS/accessibility preference to future
starts:

- Opacity-only keyframes clamp to **120 ms**.
- Delay becomes zero, iterations become one, and direction becomes `normal`.
- Any keyframes containing a non-opacity property become instant (`duration = 0`).
- Fill, play state, and easing are preserved.

`SpringRuntime::set_reduced_motion(true)` snaps new springs to their target and snaps on retarget
instead of integrating. Turning it on also snaps springs already running (they finish on the next
tick). Callers drive the animation runtime's setter from
`style::MediaContext::prefers_reduced_motion`; the frame pipeline mirrors that flag onto its springs.

### Spring ownership in the frame pipeline

`FramePipeline` owns the `SpringRuntime` (`FramePipeline::springs()`, `AppHost::springs()`); start
and retarget a style-property spring there with `SpringAnimationInput{target, property, from, to}`.
Each `tick`, after the CSS animation runtime and before dirty marking, the pipeline:

1. copies `AnimationRuntime::reduced_motion()` onto the springs;
2. drops springs whose target node is no longer in the tree (no completion callback);
3. ticks the springs with the same `dt` and merges their samples, `finished` ids, dirty bits and
   `keep_awake` into `FrameTick::animation`, so dirty mapping (`dirty_for_declarations`) and paint
   interpolation treat them like any other sample;
4. calls `RuntimeLoop::request_frame()` while a spring runs and once on the settling tick. A settled
   spring requests nothing, so static UI still idles with zero presents.

The policy applies at start/retarget time. Changing the preference does not retroactively rewrite
entries that are already running; restart or re-sync style animations when the preference changes.

## 12. Reentrancy contract

Completion callbacks run on the tick call stack. They may call back into the same runtime, with these
guarantees:

| Callback action | Result |
|---|---|
| `clear()` | Removes finished fills and all active entries; no stale samples are returned. |
| `stop(other_id)` | Drops that entry and its sample from this tick. |
| `stop_target_name(...)` | Removes its fill and any matching active entries; no stale fill sample. |
| `restart(finished_id)` | Revives the fill at zero; this tick samples zero, not the old final value. |
| `start(same target/name)` | Supersedes the finished fill; this tick samples the new animation at zero. |
| `start(new target/name)` | Adds a new entry; this tick samples it at zero while unrelated fills remain. |

Callback order follows finished-entry order. Each callback receives the id that finished. Do not
destroy the runtime from inside a callback; use `clear()` or `stop` instead.

### Completion events (animationend / transitionend)

`AnimationInput::on_complete` runs inside `tick`. Host and app code should instead register an
`AnimationEventListener` with `add_event_listener` (remove with `remove_event_listener`). `tick`
only queues an `AnimationEvent` for each naturally finished entry (`id`, `target`, `kind`,
`name`, `property`, `elapsed`); nothing is invoked from the tick loop's mutation.
`FramePipeline::tick` calls `dispatch_events()` after style/dirty collection, and requests a frame
when a listener left animations running.

- `name` is the `@keyframes` name for `AnimationKind::Animation` and the property for
  `AnimationKind::Transition` (set by `start_transition*`); `elapsed` is the active time
  (`duration * iterations`, delay excluded, the reduced-motion clamped value when clamped).
- Events fire once, in finished-entry order. `stop*`, `clear` and supersession (same
  target/property transition, same name animation) never produce an event, because the entry
  never finishes; an event already queued by a finished entry still dispatches.
- Reduced motion clamps timing at start, so a clamped animation still fires.
- Dispatch swaps the queue out and snapshots the listeners, so a listener may start, stop or
  restart animations and add/remove listeners; a listener removed earlier in the same dispatch
  is skipped. New animations are ticked next frame.

## 13. Overlay and dirty domains

`apply_animation_result(style, result, target)` applies samples for one target. Later declarations
for the same property win. `collect_animation_declarations(result)` groups declarations by target.
`dirty_for_declarations` maps properties to `Dirty` bits:

- opacity/color-ish properties (including `border-*-color`) -> paint/render; `opacity` is the one
  property whose effect composites the whole subtree, so the frame pipeline additionally marks the
  target's descendants (`declarations_dirty_paint_subtree`, paint-pass.md section 14.1);
- layout-affecting properties -> layout/paint/render;
- transform-only effects -> paint/render.

The tick result already contains the union of dirty domains from the post-callback samples.

## 14. Tests

| Executable | Coverage |
|---|---|
| `animation_runtime` | start/tick/finish, callbacks, stop/clear, wake, dirty |
| `animation_fill` | retained finals, no wake/repeat callback, supersession, cleanup |
| `animation_pause` / `animation_pause_name` | held samples, no wake, idempotent target/name counts |
| `animation_stop` / `animation_stop_name` | target/name removal, paused entries, no callbacks |
| `animation_restart` / `animation_restart_id` | active reset, fill revival, callback repeat, isolation |
| `animation_state` | active/settled counts by target/name |
| `animation_play_state` | computed-style pause/resume of active names |
| `animation_ensure` | idempotent style starts, fill skip, play state |
| `animation_sync` | add/remove reconciliation, `none`, invalid no-op |
| `animation_loop` | frame requests for active/finishing work, sleep for paused/fill/idle |
| `animation_callback` | callbacks see fills, clear/stop wins, same-name/property supersedes |
| `animation_tick_reentrancy` | post-callback samples and dirty domains |
| `animation_event` | queued/dispatched end events, no stop/supersede events, reduced motion, reentrant listeners |
| `animation_reduced_motion` | opacity clamp, instant non-opacity, spring snap |
| `animation_pipeline` | parse -> lookup -> start -> tick -> overlay |
| `animation_style` | longhands, shorthand fallback, paused starts, invalid input |
| `frame_pipeline` | animation -> target dirty, needs-style/layout, damage/redraw plan, full-redraw normalization, idle skip |

## 15. Deferred

- CSS `animation-composition` and additive animations.
- Scroll-driven animations.
- View transitions and shared elements.
- Declarative `useAnimatedValue` code generation.
- Runtime policy changes that retroactively clamp already-running animations.
- Cross-target shared-element coordination.

## 16. Render interpolation between ticks

The animation runtime advances in fixed steps; render frames may fall between them. V1 smooths
that with a paint-only interpolation of `transform`, `opacity` and color
(`ui/frame_interpolation.hpp`, `ui/frame_pipeline.hpp`; the loop contract lives in
[`game-loop.md`](game-loop.md) section 3.1.1). The runtime itself is unchanged: samples stay
per-tick, and simulation state is never mutated by the renderer.

- `FramePipeline::tick(root, dt, alpha)` runs the normal animation tick, then captures the
  paint-visible declarations of `result.animation.samples` into a `PaintSample` per target and
  rotates the previous/current pair. `alpha` is `runtime::interpolation_alpha(...)` for the render
  about to happen; it affects only paint, so dirty domains and damage are identical for every
  alpha, and the two-argument `tick(root, dt)` (no alpha) never interpolates — it paints the
  authoritative state, which keeps callers that do not drive the render rate unchanged.
- `interpolation_view()` blends the pair (`lerp(previous, current, alpha)`) and returns an
  `InterpolationPaintView`, a read-only `paint::PaintView` the paint pass applies to a local style
  copy and to the painted transform. `Node::bounds()/transform()`, damage and hit testing keep the
  tick geometry, so the runtime's overlays, dirty reporting, and `DamageTracker` need no changes.
- Only paint-visible properties are sampled: `transform`, `opacity`, `color`, `background-color`,
  `border-*-color`. Layout and text properties are never captured, which is why interpolation
  cannot request layout work. Color declarations — including `border-*-color` — map to
  `Paint | Render` in `dirty_for_declarations` (`is_layout_property` treats only border widths and
  styles as layout).
- While interpolation is active the frame is painted one tick "behind": alpha 0 repaints the
  previous tick's sample (continuous with what was on screen just before the tick), alpha 1 the
  current one. A property the current tick stops sampling is dropped rather than held, so a
  finishing animation snaps back to the caller's computed style.
- `finish_frame()` grows the accumulated damage by each transform-sampled node's painted box, so a
  damage-clipped repaint covers the space between the two tick positions.
- Tests: `fixed_step` (alpha at 0/0.5/1 and clamping), `frame_pipeline_style_sync` (sample capture,
  value/color/transform blending, layout-not-sampled, skew hold, determinism),
  `frame_pipeline_overflow` (scripted two-tick paint at 0/0.5/1, tick-geometry hit testing, damage
  coverage, settled idle, disabled switch), `animation_pipeline` (color-only dirty domains).
