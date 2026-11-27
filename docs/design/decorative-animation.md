# Decorative and ambient animation policy

- Status: proposed. The `non_essential` classification it describes is implemented; the explicit
  flag, the ambient gate and the wake bounds are not.
- Related: [ADR-019](../../decisions/ADR-019-power-aware-runtime.md) (power-aware defaults, adaptive
  quality), [ADR-034](../../decisions/ADR-034-interaction-and-overscroll-animation.md) (interaction
  animation), [`animation.md`](animation.md), [`animation-runtime.md`](animation-runtime.md),
  [`performance.md`](performance.md) section 6 (the adaptive-quality ladder),
  [`activity-and-idle.md`](activity-and-idle.md) (the deadline that triggers ambient motion),
  [`dirty-rendering.md`](dirty-rendering.md) (damage and full-redraw fallback).

Normative animation semantics stay in [`animation-v1.md`](../specs/animation-v1.md). This note covers
only the classification, its gates, and the wake bound.

## 1. The problem

Ambient motion — a logo that drifts, an easter egg, a kiosk attract loop — is animation that exists
for no functional reason. The engine currently cannot express "this animation is optional" in a way
a quality or power policy can act on, and the one classification that does exist decides the wrong
thing for this purpose.

The relevant code, read today:

```cpp
// src/ui/animation/property_dirty.cpp
bool is_essential_animation_property(std::string_view property) {
    return is_layout_property(property) || property == "transform" || property == "opacity";
}

bool is_non_essential_animation(const style::KeyframeBlock& block) { /* all props paint-only */ }
```

`AnimationRuntime::start()` stores the result in `AnimationEntry::non_essential`
(`src/ui/animation/runtime.cpp`), and adaptive quality position 6 freezes those entries
(`suspend_non_essential()`).

The consequence is stated plainly in [`performance.md`](performance.md) section 6.1: *"`transform` and
`opacity` keep running"* — they are classified **essential**. That is correct for UI motion (a
sliding panel must not stall on a slow frame) and wrong for ambient motion, which is usually exactly
a `transform` loop. A giant octopus translating across the screen would be classified essential,
would never be suspended by the ladder, and would compete with real UI for the frame budget during a
quality downgrade.

## 2. Decision: declare optionality, do not infer it

Add one explicit bit, set by whoever starts the animation, defaulting to the safe value.

```cpp
// include/tessera/ui/animation/runtime.hpp — proposed addition to AnimationInput / AnimationEntry
enum class AnimationPriority : std::uint8_t {
    Essential,   // default: never suspended by quality or power policy
    Decorative,  // suspended at quality position 6, and gated by every ambient rule in section 4
};
```

Rules:

1. **Default is `Essential`.** An inferred default is what produced today's wrong answer for ambient
   motion; a wrong default that stops a functional animation is worse than one that keeps an
   optional animation alive longer.
2. **The existing `non_essential` computation is retained** as the *style-driven* half: a keyframes
   block that animates only paint-only properties (no `transform`, no `opacity`) stays suspendable
   even when authored as `Essential`, because [`performance.md`](performance.md) section 6.1 already
   specifies that. `Decorative` is strictly *more* suspendable, never less:

   ```text
   suspendable = (priority == Decorative) || is_non_essential_animation(keyframes)
   ```

3. **Declared, not parsed from CSS.** Adding an ambient keyword to the `animation` shorthand would
   grow the stylesheet grammar for a non-CSS concept, and the shorthand already rejects unknown
   tokens. Ambient motion is started from C++ (`AnimationRuntime::start` or
   `AppHost::animations()`), where the bit lives.
4. **Transitions are never decorative.** A transition is a response to a state change; if it is not
   worth animating it should be declared `transition: none`. Keeping the bit off the transition path
   also keeps `transition_runtime.cpp` untouched.

## 3. What "suspended" must mean for an ambient entry

`AnimationRuntime` already freezes a suspended entry at its current value, keeps its elapsed time,
and stops counting it toward `keep_awake` (`tick()` computes `keep_awake` from `!frozen(entry)`).
Two consequences an ambient animation must not break:

- **A frozen ambient entry must not wake the loop.** That is already true; the risk is a consumer
  that re-arms its deadline on every tick regardless. The rule: the ambient trigger's timer is armed
  by activity edges and by nothing else.
- **Resuming must not jump.** Frozen keeps elapsed time, so a resumed octopus continues from where
  it stopped. A consumer that would rather restart must call `restart()`, which exists.

## 4. The ambient gate

Ambient motion is allowed only when *every* condition holds. Each is a fact the engine already
exposes, so the gate is a conjunction of queries and has no state of its own.

| Condition | Source | Why |
|---|---|---|
| Activity deadline has fired | [`activity-and-idle.md`](activity-and-idle.md) | the trigger |
| `powerPolicy != power-save` | `tessera::RuntimeConfig::power_policy` | a battery/kiosk preset must not gain motion |
| Not `reduced_motion` | `app::system_prefers_reduced_motion()`, `HostConfig::reduced_motion`, `AppHost::set_reduced_motion` | WCAG 2.2.2 / the user's OS setting; `AnimationRuntime::set_reduced_motion` already snaps every start |
| Not `suspend_non_essential()` | `AnimationRuntime` accessor | the quality ladder outranks ambiance |
| Window focused and visible | `WindowMetrics` / lifecycle flags | nobody sees a hidden window's octopus |
| App opted in | a config flag, default **off** | see section 6 |

The gate is re-evaluated on every edge that could change it (activity, quality flip, appearance
change, focus, power policy) and never per frame. That is the same rule the adaptive-quality stage
already follows for its probe: read at the transition, not continuously
(`src/app/quality_stage.hpp`).

Note that `set_reduced_motion()` only flips a flag (`reduced_motion_`); it does not touch an entry
that is already running. `apply_reduced_motion` fires at `start()`, so it shortens animations started
*after* the change (and sets `iterations = 1`, so they finish). An infinite ambient loop started
before the preference arrived keeps running forever unless the gate **stops** it. That is why
reduced motion is a gate condition in section 4 and not merely a start-time transform.

## 5. Cost, and the bound

This is the part that decides whether the feature is acceptable at all.

**Frame cost.** Ambient motion is a large moving quad. Every frame it moves, the damage plan carries
its old and new bounds. With `fullRedrawThreshold = 0.6` and `maxDamageRects = 16` (defaults in
[`runtime-config-v1.md`](../specs/runtime-config-v1.md)), a quad spanning more than 60 % of the
window makes every frame a full redraw. The bound is therefore geometric, not temporal:

- an ambient overlay larger than roughly half the window is a full-redraw-every-frame feature by
  construction, and must be rejected by the consumer rather than discovered in a profile;
- a smaller quad costs two damage rects per frame and stays on the partial path.

**Wake cost.** ADR-019 decision 4 requires zero presents for a settled scene. Ambient motion is an
exception, so the exception needs a bound. Proposal:

- an ambient session runs for at most `ambient_max_duration_s` (default 30 s), after which the entry
  stops and the app returns to parking;
- any activity edge ends the session immediately, with no fade-out delay longer than one transition;
- at most one ambient session per window at a time, enforced by the consumer, not the runtime;
- the session counts in `FrameDiagnostics` so the "static scene idles" acceptance test can assert
  that a run with ambient enabled and no trigger still presents zero frames.

**Decode cost.** An animated raster (sprite sheet) is not expressible: `background-image` accepts
only gradients (`url()` is rejected with a diagnostic,
[`stylesheet-property-status-v1.md`](../specs/stylesheet-property-status-v1.md)), and there is no
`background-position` animation. The paint route an animated raster would need is not specified
here; this note only assumes the entry draws through whatever route that becomes.

## 6. Opt-in

Ambient motion defaults off, in three places, all of them already existing config surfaces:

1. `tessera.json` / `RuntimeConfig` — a new field, last in the struct like the glass and cull fields
   so positional aggregate initialization keeps its meaning (`include/tessera/runtime/config.hpp`).
   Suggested name `ambientMotion` (boolean) plus `ambientMaxDurationMs` (integer, clamped).
2. Per animation — `AnimationPriority::Decorative` at the start call site.
3. Accessibility — reduced motion overrides both.

A build that ships ambient motion on by default contradicts ADR-019's "near-zero idle cost" selling
point; the default must stay off and the docs must say so.

## 7. Tests

| Behaviour | How |
|---|---|
| Default entry is Essential | start with no priority; `suspend_non_essential()` leaves it running |
| Decorative freezes at ladder position 6 | drive `AdaptiveQualityController` past position 6, assert `suspended()` counts it and `keep_awake` is false |
| Decorative with only paint-only props freezes earlier too | the existing `is_non_essential_animation` path, unchanged |
| Reduced motion stops a running decorative entry | start, then `set_reduced_motion(true)` |
| A settled scene with ambient enabled but untriggered presents zero frames | `TestApp` frame counter |
| Any activity edge ends a session | post a `KeyDown`, assert `active()` drops to 0 and no frame is requested |
| Frozen resume continues, does not restart | freeze at elapsed E, resume, assert the next sample is continuous |

## 8. Open questions

- **Does `Decorative` belong in `AnimationSpec` (the parsed form) as well as `AnimationInput`?** The
  proposal is no, per section 2 rule 3, but a future CSS-level convention (`@keyframes` prefixed with
  a reserved name) is not impossible. Decide when a second consumer wants it.
- **Should the ladder's position 6 distinguish priority tiers** (decorative suspended at 5, style
  non-essential at 6)? Only if a real case demands it; one tier is simpler and the ladder is already
  bounded at six positions.
- **Is `ambientMotion` a runtime config field or a project config field?** Both are loaded in
  different places (`src/runtime/config.cpp` vs the project loader); the runtime one is the honest
  home, because it is a behaviour, not a build input.
