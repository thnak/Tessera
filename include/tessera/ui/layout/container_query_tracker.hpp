// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_read_model.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/container_query.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::layout {

// The engine-owned observer of container queries (ADR-041 slice 8b,
// docs/design/layout-observation.md section 5.4). It remembers the size each query container was
// last styled at and, after a layout was published, marks the descendants of every container whose
// `@container` results changed style-dirty. Only a crossed condition marks anything: resizing
// inside one breakpoint costs a size comparison per container and no style work.
//
// The host registers the containers its style pass found (a computed `container-type` of `size`
// or `inline-size`) and runs `update` from `SettleHooks::engine_observe`, so it executes before
// the app observations of every settle round. UI thread only; registered nodes must outlive their
// registration.
class ContainerQueryTracker {
public:
    // `names` is the computed `container-name` (whitespace separated, empty for none).
    // `block_axis` is true for `container-type: size`, false for `inline-size`.
    void add(Node& container, bool block_axis, std::string_view names = {});
    void remove(const Node& container);
    void clear();

    // Compares each container's published content box with the size it was last styled at.
    // Returns the number of containers whose query results changed; their descendants are marked
    // Dirty::Style. A container with no published layout is skipped and stays unstyled. A container
    // seen for the first time counts as changed, because its descendants were styled without a
    // size.
    std::size_t update(const LayoutReadModel& model,
                       const std::vector<style::ContainerQuery>& queries);

    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }

    // `SceneStyleTree::ContainerSizeFn`: `context` is a `const LayoutReadModel*`.
    static bool content_size(void* context, const Node& container, style::ContainerSize& out);

private:
    struct Entry {
        Node* node = nullptr;
        bool block_axis = false;
        std::string names;
        bool styled = false;
        style::ContainerSize last;
    };

    std::vector<Entry> entries_;
};

} // namespace tessera::ui::layout
