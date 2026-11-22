// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_settle.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>

namespace tessera::ui::layout {
namespace {

// The containers of one style pass: every node whose computed `container-type` is `size` or
// `inline-size`. The walk is the style pass's own, so a container is registered with the style it
// will be queried under.
void collect_containers(const Node& node, style::SceneStyleTree& tree,
                        const style::CompiledStyleSheet& sheet, ContainerQueryTracker& tracker) {
    const style::ComputedStyle style = tree.compute(sheet, node);
    const std::string* type = style.find("container-type");
    if (type != nullptr && (*type == "size" || *type == "inline-size")) {
        const std::string* name = style.find("container-name");
        tracker.add(const_cast<Node&>(node), *type == "size",
                    name != nullptr && *name != "none" ? std::string_view(*name)
                                                       : std::string_view{});
    }
    for (const auto& child : node.children()) {
        collect_containers(*child, tree, sheet, tracker);
    }
}

} // namespace

void LayoutSettle::set_max_rounds(std::uint32_t rounds) {
    observers_.set_max_rounds(rounds);
}

void LayoutSettle::set_relayout(RelayoutFn relayout, void* context) {
    relayout_ = relayout;
    relayout_context_ = context;
}

void LayoutSettle::set_wake(WakeFn wake, void* context) {
    wake_ = wake;
    wake_context_ = context;
}

void LayoutSettle::set_logger(diagnostics::Logger* logger) {
    logger_ = logger;
}

void LayoutSettle::set_scheduler(UpdateScheduler* scheduler) {
    scheduler_ = scheduler;
}

void LayoutSettle::publish(const LayoutResult& result, float dpi_scale) {
    model_.publish(result, dpi_scale);
}

void LayoutSettle::install_container_source(style::SceneStyleTree& tree) {
    if (!gate_open_) {
        return;
    }
    tree.set_container_source(&ContainerQueryTracker::content_size, &model_);
}

void LayoutSettle::register_containers(const Node& root, style::SceneStyleTree& tree,
                                       const style::CompiledStyleSheet& sheet, bool tree_rebuilt) {
    // The tracker holds `Node*`, so a REBUILT selector tree (a changed scene shape) can leave an
    // entry pointing at a node that is gone. A pass over the rebuilt tree marks what it finds and
    // drops the rest, so a container that survived keeps the size it was last styled at: a resize
    // inside one condition still costs no engine round (issue #693 decision 2).
    const bool gate = sheet.has_container_queries();
    if (!gate) {
        tracker_.clear();
        queries_.clear();
        gate_open_ = false;
        tree.set_container_source(nullptr, nullptr);
        return;
    }
    gate_open_ = true;
    queries_ = sheet.container_queries();
    if (tree_rebuilt) {
        tracker_.begin_pass();
    }
    collect_containers(root, tree, sheet, tracker_);
    if (tree_rebuilt) {
        tracker_.end_pass();
    }
    // The style pass resolves `@container` against the published layout through the tracker's own
    // static adapter, so the two halves cannot disagree about what a container's size is.
    install_container_source(tree);
}

LayoutSettleResult LayoutSettle::run() {
    LayoutSettleResult result;
    // The gate: a stylesheet without `@container` and a scene without observations cost one boolean
    // test and one size check, and nothing else (issue #693 decision 4).
    if (!gate_open_ && observers_.size() == 0) {
        return result;
    }
    result.active = active();
    SettleHooks hooks;
    hooks.relayout = relayout_ != nullptr ? &LayoutSettle::relayout_thunk : nullptr;
    hooks.relayout_context = this;
    hooks.wake = wake_ != nullptr ? &LayoutSettle::wake_thunk : nullptr;
    hooks.wake_context = this;
    hooks.logger = logger_;
    if (gate_open_) {
        hooks.engine_observe = &LayoutSettle::engine_observe_thunk;
        hooks.engine_observe_context = this;
    }
    // A null scheduler is the documented "no app callbacks to flush" case (issue #693 decision 3),
    // not a dummy one: the Studio canvas session has none.
    result.stats = observe_and_settle(observers_, model_, scheduler_, hooks, state_);
    result.deferred = result.stats.round_limit_hit || result.stats.deferred > 0;
    return result;
}

void LayoutSettle::relayout_thunk(void* context) {
    auto* self = static_cast<LayoutSettle*>(context);
    if (self->relayout_ != nullptr) {
        self->relayout_(self->relayout_context_);
    }
}

void LayoutSettle::wake_thunk(void* context) {
    auto* self = static_cast<LayoutSettle*>(context);
    if (self->wake_ != nullptr) {
        self->wake_(self->wake_context_);
    }
}

bool LayoutSettle::engine_observe_thunk(void* context, const LayoutReadModel& model) {
    auto* self = static_cast<LayoutSettle*>(context);
    // The engine observer runs at the start of every round, before the app observations, so an app
    // callback sees sizes that already reflect the container styles (section 5.4).
    return self->tracker_.update(model, self->queries_) != 0;
}

} // namespace tessera::ui::layout
