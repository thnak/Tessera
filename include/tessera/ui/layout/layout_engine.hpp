// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/text/size_runs.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tessera::ui::text {
class TextMeasureCache;
class TextMeasurer;
} // namespace tessera::ui::text

namespace tessera::ui::layout {

namespace rhi = tessera::rendering::rhi;

// Whether a node's text payload provably paints inside its own content box (issue #490). Layout
// computes it from the same measure paint uses, so the paint-pass cull can trust a text node's box
// instead of refusing every text node. `Unknown` is the default and means "paint must stay
// conservative": a node that paints no text, a node layout could not measure, and a node whose box
// came from a `display: none`/zero-box path all stay `Unknown`.
enum class TextFit : std::uint8_t {
    Unknown,
    Fits,
    Overflows,
};

struct LayoutInput {
    std::uint64_t id = 0;
    style::ComputedStyle style;
    std::vector<LayoutInput> children;
    std::string text;
    // Paint's wrap opt-in (`PaintData::wrap == PaintWrap::Wrap`). Layout does not use it for its
    // own boxes - Yoga's measure callback decides those - but the text-fit fact must use the very
    // wrap decision paint will, so the caller forwards it here. It is part of the layout-input
    // signature because it changes the fact, which is part of the layout result.
    bool text_wrap = false;
    // Width of the first-line box a `list-style-position: inside` marker takes at the start of this
    // Text (TextStyle::first_line_indent, docs/design/list-style-inside.md); zero for any other
    // text.
    // Stored in sixteenths of a px (`text_indent_px`) so the input stays inside its size budget;
    // `inside_marker` already rounds the box up to that grid, so layout and paint agree exactly.
    std::uint16_t text_indent_q = 0;
    [[nodiscard]] float text_indent_px() const {
        return static_cast<float>(text_indent_q) / 16.0f;
    }
    // Font-size runs of the inline spans of this Text (docs/design/inline-size-runs.md): the same
    // table paint hands the measurer, so a shrunk `sub`/`sup`/`small` is measured as it is drawn.
    text::TextSizeRunsId text_runs{};
    // The signature of this subtree as the last pass that visited it computed it (zero: never).
    // A caller that retains the input between passes keeps it so an unchanged subtree can be
    // skipped (`LayoutSignatureBuilder::reuse`). It is derived data and is not part of the input's
    // meaning; `layout_signature` ignores it.
    std::uint64_t subtree_signature = 0;
};

struct LayoutViewport {
    float width = 800.0f;
    float height = 600.0f;
    float dpi_scale = 1.0f;
    // What `em` means for the element being styled: its own computed font size (issue #608).
    // The engine sets it per element; `rem` is always the 16px root.
    float font_size = 16.0f;
};

// Resolved padding plus border on each side, in logical pixels: the distance from the border box
// to the content box (docs/design/layout-observation.md section 5.3).
struct LayoutInsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    bool operator==(const LayoutInsets&) const = default;
};

struct LayoutBox {
    std::uint64_t id = 0;
    rhi::Rect bounds;
    // Border box is `bounds`; the content box is `bounds` inset by these.
    LayoutInsets content_insets = {};
    // Whether this node's text payload provably stays inside the content box paint uses (the border
    // box minus the `padding-*` longhands, NOT `content_insets`, which also counts the border).
    // `Unknown` unless the node paints text and layout measured it; see `TextFit`.
    TextFit text_fit = TextFit::Unknown;
};

struct LayoutResult {
    std::vector<LayoutBox> boxes;
};

class LayoutEngine {
public:
    // `text_cache_capacity` bounds the text measurement cache. It must comfortably exceed the
    // number of distinct text runs in a scene: the cache is a strict LRU, so a scene with more runs
    // than slots evicts the entry it is about to need again and every layout pass re-measures the
    // whole tree. That cliff is not gradual - at 1,024 labels a size-change frame costs 6,418
    // allocations, at 1,030 it costs 57,108 (issue #735). The default is sized for a large scene
    // rather than a small one, because the failure mode is a 10x regression, not a slow path.
    static constexpr std::size_t kDefaultTextCacheCapacity = 16384;

    explicit LayoutEngine(std::shared_ptr<const text::TextMeasurer> measurer = nullptr,
                          std::size_t text_cache_capacity = kDefaultTextCacheCapacity);
    ~LayoutEngine();

    LayoutEngine(LayoutEngine&&) noexcept;
    LayoutEngine& operator=(LayoutEngine&&) noexcept;
    LayoutEngine(const LayoutEngine&) = delete;
    LayoutEngine& operator=(const LayoutEngine&) = delete;

    [[nodiscard]] LayoutResult layout(const LayoutInput& root,
                                      const LayoutViewport& viewport) const;

    // Lays out one subtree in isolation, with the subtree root at (0, 0). Used for independent
    // overlays/popups; callers offset the returned boxes into parent coordinates.
    [[nodiscard]] LayoutResult layout_subtree(const LayoutInput& root, std::uint64_t id,
                                              const LayoutViewport& viewport) const;

    [[nodiscard]] std::shared_ptr<text::TextMeasureCache> text_cache() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::layout
