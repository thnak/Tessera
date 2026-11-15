// SPDX-License-Identifier: MIT
//
// The window driver's lifecycle seam (ADR-050 section 1 rule 1: layer 0 gains seams, not concepts).
//
// `WindowHost` already polls normalized `hal::WindowEvent` values and folds them into its own
// driver state (occlusion pause, accessibility snapshots, pacing). Before AF3 it dropped the
// transitions above that fold: a `Focus`, `Blur`, `Minimized`, `Occluded`, `Restored` or `Close`
// reached the driver and nothing else, which is why ADR-050's context section records "no app
// object or app lifecycle". The driver now forwards each of those to an optional sink, in the order
// it polled them, and the layer above derives the app state from them (ADR-050 section 2).
//
// What the driver forwards is exactly what it already has: one `hal::WindowEvent`, no new type and
// no service, store or app-state concept (which layer 0 must never name). This header therefore
// declares an interface only: no library is linked to implement or consume it.

#pragma once

#include "tessera/hal/window.hpp"

namespace tessera::app {

// Implemented above layer 0 (AF3: `appfw::Application`). Every call happens on the UI thread, from
// the driver's event fold, after that iteration's events have been folded into the driver state and
// before input is pumped. An implementation must not block, acquire a device, or mutate the scene:
// the driver owns the scene and the frame, and this call is a state notification.
class LifecycleSink {
public:
    LifecycleSink() = default;
    LifecycleSink(const LifecycleSink&) = delete;
    LifecycleSink& operator=(const LifecycleSink&) = delete;
    virtual ~LifecycleSink() = default;

    // One lifecycle transition, in poll order. The forwarded types are the six ADR-050 section 2
    // derives the desktop app state from: `Focus`, `Blur`, `Minimized`, `Occluded`, `Restored` and
    // `Close`. A metrics snapshot that flips occlusion/minimization is reported as the matching
    // `Occluded`/`Minimized`/`Restored` transition, because a platform (and the deterministic test
    // seam `WindowHost::apply_metrics`) reports the lifecycle through the snapshot rather than an
    // event. `sequence` is the driver's own monotonic counter, so a sink can prove the order it was
    // called in without trusting wall clock time.
    virtual void on_window_lifecycle(const hal::WindowEvent& event) = 0;
};

} // namespace tessera::app
