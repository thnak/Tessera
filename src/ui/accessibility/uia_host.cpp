// SPDX-License-Identifier: MIT
//
// Window/host wiring for the Windows UI Automation bridge: the root-provider handshake and the
// WM_GETOBJECT answer. The bridge thread owns the root object; this file never calls UIA on the
// window thread and never waits for the bridge.

#include "tessera/ui/accessibility/uia_host.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace tessera::ui::a11y {
namespace {

// One WM_GETOBJECT answer: the host owns the published provider pointer and the window thread only
// reads it. Kept out of the header so no UI-layer type gains a Win32-shaped callback.
bool answer_root_get_object(void* context, std::intptr_t object_id, void*& raw_provider) {
    auto* host = static_cast<UiaHost*>(context);
    return host != nullptr && host->answer_get_object(object_id, raw_provider);
}

} // namespace

UiaHost::UiaHost(UiaHostConfig config, std::unique_ptr<UiaApartment> apartment,
                 std::unique_ptr<UiaPlatform> platform)
    : config_(config),
      platform_(platform != nullptr ? std::move(platform) : create_platform_uia()),
      bridge_(config_.bridge, std::move(apartment)) {}

UiaHost::~UiaHost() {
    stop();
}

std::expected<void, UiaError> UiaHost::start() {
    if (platform_ == nullptr) {
        // A build without a UIA platform seam: accessibility stays off with a diagnostic, the app
        // still runs (spec section 7).
        return std::unexpected(UiaError::UnsupportedPlatform);
    }
    // Hooks capture `this`; they run on the bridge thread and the bridge is always stopped (and
    // joined) before this host is destroyed.
    UiaBridgeHooks hooks;
    hooks.on_start = [this](UiaProvider& provider) { on_bridge_start(provider); };
    hooks.on_stop = [this](UiaProvider& provider) { on_bridge_stop(provider); };
    bridge_.set_hooks(std::move(hooks));
    return bridge_.start();
}

void UiaHost::stop() {
    // `stop` joins the bridge worker, so no hook can run after it returns. The root pointer is
    // cleared before the pointer is released (the stop hook does that on the bridge thread).
    bridge_.stop();
    root_.store(nullptr, std::memory_order_release);
    // A flush job that was queued when the worker stopped never ran; let the next start queue one.
    flush_posted_.store(false, std::memory_order_release);
}

bool UiaHost::ready() const {
    return root_.load(std::memory_order_acquire) != nullptr;
}

bool UiaHost::answer_get_object(std::intptr_t object_id, void*& raw_provider) const {
    // Called from the window procedure: one atomic load, no lock, no COM. A not-ready bridge
    // returns false so the window keeps its default accessibility provider instead of failing.
    if (object_id != kUiaRootObjectId) {
        return false;
    }
    void* root = root_.load(std::memory_order_acquire);
    if (root == nullptr) {
        return false;
    }
    raw_provider = root;
    return true;
}

void UiaHost::set_host_handle(hal::AccessibilityHostHandle handle) {
    // The platform stores the token; nothing is created until a client asks for the host element on
    // the bridge thread. Window thread only, so it cannot race the bridge's own root state.
    if (platform_ != nullptr) {
        platform_->set_host_handle(handle);
    }
}

UiaHostElementStatus UiaHost::host_element_status() const {
    return platform_ != nullptr ? platform_->host_element_status() : UiaHostElementStatus{};
}

void UiaHost::mark_dirty() {
    dirty_.store(true, std::memory_order_release);
}

bool UiaHost::dirty() const {
    return dirty_.load(std::memory_order_acquire);
}

UiaHostTickStats UiaHost::tick(const SnapshotSource& source, const ActionHandler& handler) {
    UiaHostTickStats stats;
    if (dirty_.load(std::memory_order_acquire)) {
        // Build on the UI thread; the bridge only ever sees the immutable copy. A throwing source
        // keeps the previous tree published (and the host dirty) instead of shipping a half-built
        // snapshot, and the failure is reported as a value.
        bool built = true;
        try {
            if (source) {
                source(next_);
            } else {
                next_.clear();
            }
        } catch (...) {
            // Boundary: host callbacks are untrusted UI code; never unwind into the runtime loop.
            built = false;
        }
        if (built) {
            // Diff against the last published tree while both are still intact; the events are
            // notifications only, so the snapshot publish below is what a client actually reads.
            const UiaEventDiff diff = queue_snapshot_diff(previous_, next_, events_);
            stats.events_queued = diff.queued;
            bridge_.publish(next_);
            // Retain the published tree for the next diff, reusing the buffers both ways.
            previous_.nodes.swap(next_.nodes);
            dirty_.store(false, std::memory_order_release);
            stats.published = true;
        } else {
            stats.build_failed = true;
            next_.clear();
        }
    }

    // Raise the queued events on the bridge thread, at most one flush job in flight. A full job
    // ring defers the flush to a later tick instead of waiting for it here.
    if (events_.size() > 0) {
        bool expected = false;
        if (flush_posted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            const auto posted = bridge_.post([this](UiaProvider&) { flush_events(); });
            if (!posted) {
                flush_posted_.store(false, std::memory_order_release);
            }
        }
    }
    stats.events_pending = events_.size();

    if (handler) {
        // Tick-boundary drain: a bounded batch per tick, oldest first. The action ring stays
        // bounded by the provider, so a client that spams still cannot starve the frame.
        const std::size_t budget = std::min(config_.max_actions_per_tick, kMaxPendingUiaActions);
        std::array<UiaActionRequest, kMaxPendingUiaActions> batch{};
        const std::size_t taken = bridge_.take_actions(std::span(batch.data(), budget));
        for (std::size_t index = 0; index < taken; ++index) {
            try {
                handler(batch[index]);
            } catch (...) {
                // Boundary: an accessibility action runs host code; a throw ends this tick's
                // batch. The failed action is dropped (never retried) and reported as a value.
                stats.actions_applied = index;
                stats.actions_failed = 1;
                stats.actions_deferred = bridge_.queued_actions();
                return stats;
            }
        }
        stats.actions_applied = taken;
    }
    stats.actions_deferred = bridge_.queued_actions();
    return stats;
}

UiaBridge& UiaHost::bridge() {
    return bridge_;
}

const UiaBridge& UiaHost::bridge() const {
    return bridge_;
}

UiaError UiaHost::error() const {
    return bridge_.error();
}

std::uint64_t UiaHost::raised_batches() const {
    return raised_batches_.load(std::memory_order_acquire);
}

std::uint64_t UiaHost::failed_raises() const {
    return failed_raises_.load(std::memory_order_acquire);
}

std::size_t UiaHost::pending_events() const {
    return events_.size();
}

void UiaHost::flush_events() {
    // Bridge thread: one bounded batch per posted job, so a burst of changes cannot hold the
    // bridge for longer than one provider call per event.
    flush_posted_.store(false, std::memory_order_release);
    std::array<UiaEvent, kMaxQueuedUiaEvents> batch{};
    const std::size_t taken = events_.take(batch);
    if (taken == 0 || platform_ == nullptr) {
        return;
    }
    try {
        const UiaError raised = platform_->raise(std::span(batch.data(), taken));
        if (raised == UiaError::None) {
            raised_batches_.fetch_add(1, std::memory_order_relaxed);
        } else {
            failed_raises_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        // Boundary: a platform raiser must not unwind into the bridge loop.
        failed_raises_.fetch_add(1, std::memory_order_relaxed);
    }
}

void UiaHost::on_bridge_start(UiaProvider&) {
    // Bridge thread, after `CoInitializeEx`. Creating the provider here is what makes the object
    // bridge-thread-owned: its COM methods marshal to this same thread and run inline.
    void* root = platform_->create_root(bridge_);
    root_.store(root, std::memory_order_release);
}

void UiaHost::on_bridge_stop(UiaProvider&) {
    // Stop answering before releasing: a WM_GETOBJECT racing shutdown must see "not ready" rather
    // than a dangling provider.
    root_.store(nullptr, std::memory_order_release);
    platform_->destroy_root();
}

hal::AccessibilityRoot make_window_root_provider(UiaHost& host) {
    // Same context and callback on every call, so reinstalling is a no-op for the window.
    return {&host, &answer_root_get_object};
}

} // namespace tessera::ui::a11y
