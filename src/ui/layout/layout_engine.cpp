// SPDX-License-Identifier: MIT
//
// The public layout seam. `layout` runs the retained Yoga tree (layout_retained.cpp), which keeps
// one `YGNode` per node id across frames and re-applies a node's style only when its layout digest
// changed (issue #735, slice D). `layout_subtree` lays one subtree out in isolation through the
// from-scratch path (layout_cold.cpp), because a subtree laid out on its own must not disturb the
// retained tree's shape.

#include "layout_cold.hpp"
#include "layout_retained.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace tessera::ui::layout {
namespace {

const LayoutInput* find_input(const LayoutInput& root, std::uint64_t id) {
    if (root.id == id) {
        return &root;
    }
    for (const LayoutInput& child : root.children) {
        if (const LayoutInput* found = find_input(child, id)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

struct LayoutEngine::Impl {
    Impl(std::shared_ptr<const text::TextMeasurer> text_measurer, std::size_t cache_capacity)
        : cache(
              std::make_shared<text::TextMeasureCache>(std::move(text_measurer), cache_capacity)) {}

    std::shared_ptr<text::TextMeasureCache> cache;
    // `layout` is const but the retained tree is stateful by design: it is the cache. `mutable` is
    // the honest spelling of that, and the engine is not shared across threads.
    mutable detail::RetainedTree retained;
};

LayoutEngine::LayoutEngine(std::shared_ptr<const text::TextMeasurer> measurer,
                           std::size_t text_cache_capacity) {
    if (!measurer) {
        measurer = std::make_shared<text::StubTextMeasurer>();
    }
    impl_ = std::make_unique<Impl>(std::move(measurer), text_cache_capacity);
}

LayoutEngine::~LayoutEngine() = default;
LayoutEngine::LayoutEngine(LayoutEngine&&) noexcept = default;
LayoutEngine& LayoutEngine::operator=(LayoutEngine&&) noexcept = default;

LayoutResult LayoutEngine::layout(const LayoutInput& root, const LayoutViewport& viewport) const {
    return impl_->retained.layout(root, viewport, *impl_->cache);
}

LayoutResult LayoutEngine::layout_subtree(const LayoutInput& root, std::uint64_t id,
                                          const LayoutViewport& viewport) const {
    const LayoutInput* subtree = find_input(root, id);
    if (subtree == nullptr) {
        return {};
    }
    return detail::layout_cold(*subtree, viewport, viewport.width, viewport.height, *impl_->cache,
                               /*as_grid_item=*/false);
}

std::shared_ptr<text::TextMeasureCache> LayoutEngine::text_cache() const {
    return impl_->cache;
}

} // namespace tessera::ui::layout
