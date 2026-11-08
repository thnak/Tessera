// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextMeasurer;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_cache: " << message << "\n";
    }
    return condition;
}

class CountingMeasurer final : public TextMeasurer {
public:
    TextMetrics measure(std::string_view text, const TextStyle&,
                        const TextConstraints&) const override {
        ++calls;
        return TextMetrics{static_cast<float>(text.size()) * 8.0f, 16.0f, 12.0f, 1};
    }

    mutable std::size_t calls = 0;
};

ComputedStyle text_style() {
    ComputedStyle style;
    style.set("font-size", "16px");
    style.set("line-height", "20px");
    return style;
}

LayoutInput text_layout(std::string_view text) {
    LayoutInput root;
    root.id = 1;
    root.style.set("display", "flex");
    root.style.set("flex-direction", "column");
    root.style.set("align-items", "flex-start");
    root.style.set("width", "100px");
    root.style.set("height", "100px");

    LayoutInput child;
    child.id = 2;
    child.style = text_style();
    child.text = std::string(text);
    root.children.push_back(std::move(child));
    return root;
}

bool same_boxes(const LayoutResult& lhs, const LayoutResult& rhs) {
    if (lhs.boxes.size() != rhs.boxes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.boxes.size(); ++i) {
        const auto& a = lhs.boxes[i];
        const auto& b = rhs.boxes[i];
        if (a.id != b.id || std::abs(a.bounds.x - b.bounds.x) > 0.001f ||
            std::abs(a.bounds.y - b.bounds.y) > 0.001f ||
            std::abs(a.bounds.width - b.bounds.width) > 0.001f ||
            std::abs(a.bounds.height - b.bounds.height) > 0.001f) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    auto measurer = std::make_shared<CountingMeasurer>();
    LayoutEngine engine(measurer, 8);
    const LayoutViewport viewport{100.0f, 100.0f};
    const LayoutInput input = text_layout("hello");

    const LayoutResult first = engine.layout(input, viewport);
    const std::size_t first_calls = measurer->calls;
    const LayoutResult second = engine.layout(input, viewport);
    if (!expect(first_calls > 0, "first layout measured") ||
        !expect(measurer->calls == first_calls, "second layout cache hit") ||
        !expect(same_boxes(first, second), "boxes unchanged") ||
        !expect(engine.text_cache() != nullptr, "cache exposed") ||
        !expect(engine.text_cache()->capacity() == 8, "capacity configured") ||
        !expect(engine.text_cache()->stats().misses > 0, "misses recorded") ||
        !expect(engine.text_cache()->stats().hits > 0, "hits recorded")) {
        return 1;
    }

    const LayoutResult changed = engine.layout(text_layout("world"), viewport);
    if (!expect(measurer->calls > first_calls, "new text measured") ||
        !expect(changed.boxes.size() == first.boxes.size(), "new layout boxes")) {
        return 2;
    }

    bool threw = false;
    try {
        LayoutEngine invalid(measurer, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero cache capacity rejected")) {
        return 3;
    }

    std::cout << "layout_cache: ok\n";
    return 0;
}
