// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_handoff.hpp"

#include <algorithm>

namespace tessera::ui::a11y {

SnapshotHandoff::SnapshotHandoff() {
    pending_.nodes.reserve(kMaxNodes);
}

void SnapshotHandoff::publish(const A11ySnapshot& snapshot) {
    std::lock_guard lock(mutex_);
    if (has_pending_) {
        ++stats_.dropped;
    }
    // `pending_` keeps its reserved capacity across every take, so the UI-thread publish path does
    // not allocate once the slot has been sized.
    pending_.nodes.assign(snapshot.nodes.begin(), snapshot.nodes.end());
    has_pending_ = true;
    ++stats_.published;
}

bool SnapshotHandoff::take(A11ySnapshot& out) {
    std::lock_guard lock(mutex_);
    if (!has_pending_) {
        return false;
    }
    // Copy into the caller's reusable buffer instead of swapping the reserved slot away, so the
    // next publish still writes into preallocated storage.
    out.nodes.resize(pending_.nodes.size());
    std::copy(pending_.nodes.begin(), pending_.nodes.end(), out.nodes.begin());
    has_pending_ = false;
    ++stats_.taken;
    return true;
}

bool SnapshotHandoff::has_pending() const {
    std::lock_guard lock(mutex_);
    return has_pending_;
}

SnapshotHandoffStats SnapshotHandoff::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::uint64_t SnapshotHandoff::published() const {
    std::lock_guard lock(mutex_);
    return stats_.published;
}

void SnapshotHandoff::clear() {
    std::lock_guard lock(mutex_);
    pending_.nodes.clear();
    has_pending_ = false;
}

} // namespace tessera::ui::a11y
