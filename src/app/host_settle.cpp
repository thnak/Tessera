// SPDX-License-Identifier: MIT
//
// The ADR-041 settle loop's host half (issue #693): the one restyle-and-layout sequence a frame and
// a settle round share, the publish-and-run step, and the two hooks the loop calls back through.
// Split out of `host.cpp`, which is at its size budget.

#include "host_impl.hpp"

namespace tessera::app {

void AppHost::Impl::init_settle() {
    // The relayout hook is the frame's own restyle-and-layout sequence, and the wake hook asks the
    // runtime loop for the next tick. The round budget comes from the runtime config
    // (`layoutObservationMaxRounds`), clamped by the component.
    settle.set_max_rounds(static_cast<std::uint32_t>(config.layout_observation_max_rounds));
    settle.set_relayout(&Impl::settle_relayout, this);
    settle.set_wake(&Impl::settle_wake, this);
    settle.set_scheduler(scheduler);
}

// The one restyle-and-layout sequence (issue #693 decision 1). The frame and the settle loop's
// relayout hook both call it, so a settle round cannot do less than a frame: a hook that only
// re-laid-out would paint a stale style generation or replay a stale paint cache.
void AppHost::Impl::restyle_and_layout(ui::frame::FrameTick& tick,
                                       ui::paint::SceneStyleSyncResult& sync, bool& style_rebuilt) {
    const bool incremental = prepare_style_tree();
    // A restyled selector tree is a new style generation. The paint cache key holds a node's own
    // dirty flags, not its computed style, so a node that stayed Clean can still resolve a
    // different style now (an inherited value, an ancestor/sibling selector, a restyled root or a
    // new theme). Dropping every record is the only sound answer; replaying one would paint the
    // previous generation and differ from a cold full paint.
    pipeline->invalidate_paint_cache();
    style_rebuilt = true;
    restyle_memo.begin(*provider, incremental);
    run_layout_pass(restyle_memo);
    sync = pipeline->sync_style(root, restyle_memo, tick, true);
    restyle_memo.end();
    style_tree_populated = true;
    pipeline->sample_started_animations(root);
    // The tracker holds `Node*` and a REBUILT selector tree can change the shape, so the containers
    // are re-registered from this style pass (issue #693 decision 2). A restyle in place keeps the
    // tracker's entries, so a container remembers the size it was last styled at.
    settle.register_containers(root, *tree, sheet, !incremental);
}

void AppHost::Impl::settle_layout() {
    // Publish the committed layout, then run the bounded settle loop. A stylesheet without
    // `@container` and a scene without observations return from `run` after one boolean test.
    settle.publish(last_layout, config.dpi_scale);
    const ui::layout::LayoutSettleResult result = settle.run();
    if (result.deferred) {
        // Correctness wins over idling: a stale size must not stay on screen, so the loop is woken
        // for the next tick (the helper already asked through its wake hook; this is the host's own
        // render-dirty mark, which the on-demand loop reads).
        loop.mark_render_dirty();
    }
}

void AppHost::Impl::settle_relayout(void* context) {
    auto* self = static_cast<Impl*>(context);
    // The relayout hook runs inside the settle loop, so it must not re-enter the loop: it does the
    // frame's restyle-and-layout and publishes, and the loop continues with the next round.
    ui::frame::FrameTick tick;
    ui::paint::SceneStyleSyncResult sync;
    bool style_rebuilt = false;
    self->restyle_and_layout(tick, sync, style_rebuilt);
    self->settle.publish(self->last_layout, self->config.dpi_scale);
}

void AppHost::Impl::settle_wake(void* context) {
    static_cast<Impl*>(context)->loop.request_frame();
}

} // namespace tessera::app
