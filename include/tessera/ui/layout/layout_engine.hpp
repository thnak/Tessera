// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/style/cascade.hpp"

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
};

struct LayoutViewport {
    float width = 800.0f;
    float height = 600.0f;
    float dpi_scale = 1.0f;
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
    explicit LayoutEngine(std::shared_ptr<const text::TextMeasurer> measurer = nullptr,
                          std::size_t text_cache_capacity = 1024);
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
