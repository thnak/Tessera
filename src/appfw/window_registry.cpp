// SPDX-License-Identifier: MIT
//
// The app's window set (issue #180). See `window_registry.hpp` for the contract; this file owns the
// table and the two folds, and nothing else.

#include "tessera/appfw/window_registry.hpp"

#include <utility>

namespace tessera::appfw {

WindowRegistry::WindowRegistry(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {
    ids_ = std::make_unique<WindowId[]>(capacity_);
    lifecycles_ = std::make_unique<WindowLifecycle[]>(capacity_);
}

WindowRegistry::~WindowRegistry() = default;

std::size_t WindowRegistry::index_of(WindowId id) const noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
        if (ids_[index] == id) {
            return index;
        }
    }
    return count_;
}

std::optional<WindowId> WindowRegistry::attach(WindowLifecycle initial) {
    if (count_ >= capacity_) {
        return std::nullopt;
    }
    const WindowId id{next_id_++};
    ids_[count_] = id;
    lifecycles_[count_] = initial;
    ++count_;
    return id;
}

bool WindowRegistry::detach(WindowId id) {
    const std::size_t index = index_of(id);
    if (index >= count_) {
        return false;
    }
    // Preserve attach order by shifting the tail down: `windows()` and `ids()` stay parallel and
    // the aggregate walk keeps its stable order, which is what makes a two-window test's expected
    // sequence readable.
    for (std::size_t move = index; move + 1 < count_; ++move) {
        ids_[move] = ids_[move + 1];
        lifecycles_[move] = lifecycles_[move + 1];
    }
    --count_;
    return true;
}

bool WindowRegistry::fold(WindowId id, LifecycleSignal signal) {
    const std::size_t index = index_of(id);
    if (index >= count_) {
        return false;
    }
    lifecycles_[index] = fold_window_lifecycle(lifecycles_[index], signal);
    return true;
}

bool WindowRegistry::apply_snapshot(WindowId id, bool focused, bool occluded, bool minimized) {
    const std::size_t index = index_of(id);
    if (index >= count_) {
        return false;
    }
    WindowLifecycle& window = lifecycles_[index];
    window.focused = focused;
    window.occluded = occluded;
    window.minimized = minimized;
    // The same rule `Application::apply_window_snapshot` applies to one window: a snapshot that
    // reports a visible, focused window clears the closed bit, because the platform is showing it
    // again and the ADR derives `Hidden` from "no visible window", not from a stale close request.
    window.visible = !(occluded || minimized);
    if (window.visible && focused) {
        window.closed = false;
    }
    return true;
}

const WindowLifecycle* WindowRegistry::find(WindowId id) const noexcept {
    const std::size_t index = index_of(id);
    return index < count_ ? &lifecycles_[index] : nullptr;
}

std::size_t WindowRegistry::open_count() const noexcept {
    std::size_t open = 0;
    for (std::size_t index = 0; index < count_; ++index) {
        if (!lifecycles_[index].closed) {
            ++open;
        }
    }
    return open;
}

} // namespace tessera::appfw
