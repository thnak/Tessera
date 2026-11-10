// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"

#include <cstdint>
#include <mutex>
#include <string_view>

namespace tessera::ui::a11y {

// Counters for one handoff. `dropped` counts publishes that replaced a snapshot the bridge had not
// consumed yet; the newer snapshot wins because the bridge always serves the latest tree.
struct SnapshotHandoffStats {
    std::uint64_t published = 0;
    std::uint64_t taken = 0;
    std::uint64_t dropped = 0;
};

// Single-producer/single-consumer snapshot handoff (docs/specs/accessibility-v1.md section 10,
// ADR-021). The UI thread calls `publish`; the bridge thread calls `take`. There is exactly one
// slot, so publishing while a snapshot is waiting replaces it and increments `dropped` (drop-oldest
// keeps the newest tree, never an unbounded backlog). The slot keeps its reserved capacity, and
// `take` copies into the consumer's reused buffer, so a steady-state publish/take cycle allocates
// nothing.
//
// The mutex is held for one bounded vector copy, never across I/O or provider work; the UI thread
// never waits on the bridge thread here.
class SnapshotHandoff {
public:
    SnapshotHandoff();

    // UI thread: publishes `snapshot` as the latest tree. Bounded by kMaxNodes; never blocks on
    // the bridge.
    void publish(const A11ySnapshot& snapshot);

    // Bridge thread: returns false when no snapshot is waiting, otherwise swaps the newest one into
    // `out` (the previous contents of `out` become the handoff buffer).
    [[nodiscard]] bool take(A11ySnapshot& out);

    [[nodiscard]] bool has_pending() const;
    [[nodiscard]] SnapshotHandoffStats stats() const;
    // Number of snapshots accepted since construction; strictly increasing per publish.
    [[nodiscard]] std::uint64_t published() const;

    void clear();

private:
    mutable std::mutex mutex_;
    A11ySnapshot pending_;
    bool has_pending_ = false;
    SnapshotHandoffStats stats_{};
};

} // namespace tessera::ui::a11y
