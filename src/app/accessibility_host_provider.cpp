// SPDX-License-Identifier: MIT
//
// The pull-side half of the WindowHost accessibility controller (issue #189): the Android shape.
//
// Android is the one platform whose accessibility API is synchronous. `AccessibilityNodeProvider`
// is asked "describe virtual view 7" on the UI thread and must answer before it returns; there is
// no bridge thread to push a tree to. So this backend does not publish into a bridge - it keeps a
// `SnapshotProvider` current and drains the actions the platform queued into it.
//
// Everything bounded stays bounded: a tick builds at most one snapshot (and only when dirty), the
// action drain is capped by the provider's own queue, and the provider's mutex is held for one
// bounded copy rather than across a build. The platform's query path never touches this controller.

#include "accessibility_host_impl.hpp"

#include <array>

namespace tessera::app {
namespace {

namespace a11y = tessera::ui::a11y;

} // namespace

bool AccessibilityHost::Impl::start_provider() {
    status.backend = AccessibilityBackend::Provider;
    if (config.provider == nullptr) {
        // A build or a shell with nowhere to publish. This is the documented "accessibility off"
        // outcome, not a failure: the window keeps running and a screen reader sees one plain view.
        disable("provider", "no provider was installed");
        return false;
    }
    provider = config.provider;
    status.enabled = true;
    // The first tree must reach the provider: the window already has content by the time the
    // controller exists, and a screen reader's first query should not find an empty tree.
    dirty = true;
    return true;
}

bool AccessibilityHost::Impl::tick_provider() {
    if (provider == nullptr) {
        return false;
    }
    bool published = false;
    if (dirty) {
        try {
            build(snapshot);
        } catch (...) {
            // Boundary: an untrusted snapshot source. Keep the previous tree served and stay dirty
            // so a later tick retries; report it as a value rather than throwing out of the tick.
            ++status.failed_builds;
            return false;
        }
        provider->publish(snapshot);
        status.nodes = snapshot.nodes.size();
        ++status.publishes;
        dirty = false;
        published = true;
    }

    // Tick-boundary action drain, bounded by the provider's own queue. The platform queues from its
    // thread; the window driver executes here, on the thread that owns the scene.
    std::array<a11y::ProviderAction, a11y::kMaxProviderActions> batch{};
    const std::size_t taken = provider->take_actions(batch);
    for (std::size_t index = 0; index < taken; ++index) {
        AccessibilityAction action;
        action.node = batch[index].node;
        action.action = batch[index].action;
        // The value-setting actions carry their payload in the queue entry; it is copied here so the
        // handler can read it after the provider's entry is reused.
        action.text = batch[index].text_of();
        if (action.node != 0) {
            apply_actions(std::span<const AccessibilityAction>(&action, 1));
            ++status.actions;
        }
    }
    return published;
}

} // namespace tessera::app
