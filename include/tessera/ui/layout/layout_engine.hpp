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

struct LayoutInput {
    std::uint64_t id = 0;
    style::ComputedStyle style;
    std::vector<LayoutInput> children;
    std::string text;
};

struct LayoutViewport {
    float width = 800.0f;
    float height = 600.0f;
    float dpi_scale = 1.0f;
};

struct LayoutBox {
    std::uint64_t id = 0;
    rhi::Rect bounds;
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

    [[nodiscard]] std::shared_ptr<text::TextMeasureCache> text_cache() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::layout
