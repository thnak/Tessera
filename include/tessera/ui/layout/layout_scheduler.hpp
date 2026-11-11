// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::ui::layout {

// Incremental relayout policy: reuse the last layout result while the layout-input signature (see
// layout_signature.hpp) is unchanged. The signature covers the tree shape, the layout-affecting
// computed longhands, the text and the viewport, so a hit is equivalent to a reflow for the same
// inputs even when the caller reported layout dirt (a spurious report, e.g. from a paint-only
// style change). Yoga has no partial-layout API, so any real signature change recomputes the tree.
class LayoutScheduler {
public:
    explicit LayoutScheduler(LayoutEngine& engine);

    // Reflows only when the signature changed or the cache is invalid. `needs_layout` is a hint:
    // dirt with an unchanged signature is a cache hit, because a changed input cannot hide behind
    // an equal signature.
    [[nodiscard]] const LayoutResult& update(const LayoutInput& root,
                                             const LayoutViewport& viewport, bool needs_layout);
    [[nodiscard]] const LayoutResult& current() const {
        return last_;
    }
    [[nodiscard]] bool valid() const {
        return valid_;
    }
    [[nodiscard]] std::size_t layout_runs() const {
        return layout_runs_;
    }
    // Updates served from the cache: clean frames plus reports whose dirt did not change the
    // signature.
    [[nodiscard]] std::size_t cache_hits() const {
        return cache_hits_;
    }
    // Relayouts an independent subtree (overlay/popup) and patches its boxes into the cached
    // result, keeping sibling boxes untouched. Returns false when the cache is invalid or the
    // subtree root has no previous box. Structural removals inside the subtree still require
    // invalidate()/a full update; style/size changes are the intended case.
    //
    // A successful patch drops the signature, because the patched boxes are a subtree snapshot
    // (root at the origin, resolved against the subtree's own width) rather than a full reflow.
    // The next update therefore reflows instead of reusing an approximate patch.
    bool relayout_subtree(const LayoutInput& root, std::uint64_t id,
                          const LayoutViewport& viewport);

    [[nodiscard]] std::size_t partial_runs() const {
        return partial_runs_;
    }

    // Signature cached with the last full reflow. Only meaningful while `has_signature()` is true.
    [[nodiscard]] std::uint64_t signature() const {
        return signature_;
    }
    [[nodiscard]] bool has_signature() const {
        return signature_valid_;
    }

    // Drops the cached result and the signature; the next update always reflows.
    void invalidate();

private:
    LayoutEngine* engine_ = nullptr;
    LayoutResult last_;
    bool valid_ = false;
    std::uint64_t signature_ = 0;
    bool signature_valid_ = false;
    std::size_t layout_runs_ = 0;
    std::size_t cache_hits_ = 0;
    std::size_t partial_runs_ = 0;
};

} // namespace tessera::ui::layout
