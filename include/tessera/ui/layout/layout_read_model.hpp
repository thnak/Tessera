// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace tessera::ui::layout {

// A value snapshot of one node's committed layout, in logical pixels
// (docs/design/layout-observation.md section 5.9).
struct LayoutInfo {
    rhi::Rect border_box;    // root-relative, untransformed (from LayoutResult)
    rhi::Rect content_box;   // border box inset by padding and border
    rhi::Rect visual_bounds; // Node::world_bounds(): transform and scroll applied
    float dpi_scale = 1.0f;
    std::uint64_t generation = 0; // the publish that produced the snapshot
};

// The query side of layout: the host publishes each finished layout once, and anything can then
// read sizes without running or forcing layout (so reads never thrash). Reading and writing are
// separate on purpose: publishing is the only mutation, and it happens at one defined point after
// `apply_layout`.
//
// Storage is one sorted id table that keeps its capacity between publishes, so a steady tree
// publishes without allocating. UI thread only: `publish` and `find` assert the owner thread in
// debug builds, like `UpdateScheduler`.
class LayoutReadModel {
public:
    // Replaces the committed layout and bumps the generation. `dpi_scale` is the window's device
    // scale for this layout.
    void publish(const LayoutResult& result, float dpi_scale);

    // The committed layout of `node`, or nullopt when it was not part of the last published result
    // (not yet laid out, or added since). `visual_bounds` is read from the node at call time, so it
    // reflects the current transform and scroll offset.
    [[nodiscard]] std::optional<LayoutInfo> find(const Node& node) const;

    // Number of publishes so far; 0 before the first. Equal generations mean equal layout.
    [[nodiscard]] std::uint64_t generation() const {
        return generation_;
    }
    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }

private:
    struct Entry {
        std::uint64_t id = 0;
        rhi::Rect border_box;
        rhi::Rect content_box;
    };

    std::vector<Entry> entries_; // sorted by id
    float dpi_scale_ = 1.0f;
    std::uint64_t generation_ = 0;
    std::thread::id owner_ = std::this_thread::get_id();
};

} // namespace tessera::ui::layout
