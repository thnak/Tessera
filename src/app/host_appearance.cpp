// SPDX-License-Identifier: MIT
//
// The host half of issue #187: applying a new OS appearance snapshot at run time.
//
// The acceptance is "the host updates `reducedMotion` AND the media queries", and those are two
// different mechanisms: `reducedMotion` is host state the animation/spring/scheduler runtimes read,
// while the media queries are resolved ONCE per cascade generation against `media_context`.
// Applying only the first half leaves a stylesheet whose `@media (prefers-reduced-motion: reduce)`
// rules keep the applicability they had at startup, which is exactly the bug this file exists to
// prevent.
//
// Both preferences can move the cascade, so the rebuild is keyed on the media context moving at all
// (either field), not on the color scheme alone. The rebuild itself is the existing ADR-048 resolve
// step: the media-independent prepared data is re-resolved against the new context, the style
// generation is rebuilt and the paint cache dropped, exactly like a theme switch. No second "theme
// changed" mechanism, and no re-parse of CSS.
//
// Split out of `host.cpp` (which is at its size budget) so the rule and its two caveats are stated
// in one place.

#include "host_impl.hpp"

namespace tessera::app {

// A new OS appearance snapshot (issue #187). The reduced-motion half goes through the existing
// `set_reduced_motion` path so every consumer (animations, springs, the scheduler, scroll clamping)
// sees it; a media-affecting change re-resolves the stylesheet against the new media context.
void AppHost::set_appearance(const hal::AppearancePreference& appearance) {
    Impl& state = *impl_;
    if (hal::appearance_changed(state.appearance, appearance)) {
        state.appearance = appearance;
        const ui::style::MediaContext next =
            ui::style::with_appearance(state.media_context, appearance);
        // Which field moved does not matter: an `@media` rule can be gated on either one, so the
        // cascade is stale as soon as either moved. Comparing the RESOLVED context (not the raw
        // preference) is what makes a change that only touched an assumption bit - a measurement
        // replacing the documented fallback with the same value - cost no rebuild.
        const bool media_moved =
            next.color_scheme != state.media_context.color_scheme ||
            next.prefers_reduced_motion != state.media_context.prefers_reduced_motion;
        state.media_context = next;
        if (media_moved && state.base_prepared) {
            // Re-evaluating every rule is the only correct response: the same node can resolve a
            // different computed style without its own dirty flag (the documented reason a
            // generation change invalidates everything), and a media rule may change any property.
            state.apply_style_generation(ui::Dirty::Style | ui::Dirty::Paint | ui::Dirty::Render);
        }
        // A host built through the pre-resolved `CompiledStyleSheet` constructor keeps no prepared
        // data to re-resolve (`base_prepared` is null), so its cascade cannot follow a later media
        // change. That is a property of that constructor, not of this rule: the snapshot and the
        // media context still move, so a caller that resolves its own sheet sees the new values.
    }
    // The motion flag is applied even when only the assumption bit moved: a measurement replacing a
    // guess with the same value is not a visual change, but the call is a cheap idempotent set.
    set_reduced_motion(appearance.reduced_motion);
}

} // namespace tessera::app
