// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_measure_cache.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextMeasureCache;
using tessera::ui::text::TextMeasurer;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_measure_cache: " << message << "\n";
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

} // namespace

int main() {
    auto measurer = std::make_shared<CountingMeasurer>();
    TextMeasureCache cache(measurer, 2);
    TextStyle style;
    TextConstraints constraints;

    const TextMetrics first = cache.measure("a", style, constraints);
    if (!expect(first.width == 8.0f, "first metrics") || !expect(cache.size() == 1, "first size") ||
        !expect(measurer->calls == 1, "first call") ||
        !expect(cache.stats().misses == 1, "first miss") ||
        !expect(cache.stats().hits == 0, "first no hit")) {
        return 1;
    }

    const TextMetrics hit = cache.measure("a", style, constraints);
    if (!expect(hit.width == 8.0f, "hit metrics") || !expect(measurer->calls == 1, "hit call") ||
        !expect(cache.stats().hits == 1, "hit count")) {
        return 2;
    }

    cache.measure("b", style, constraints);
    cache.measure("a", style, constraints);
    cache.measure("c", style, constraints);
    if (!expect(cache.size() == 2, "capacity bounded") ||
        !expect(cache.stats().evictions == 1, "lru eviction") ||
        !expect(cache.stats().hits == 2, "two hits") ||
        !expect(cache.stats().misses == 3, "three misses")) {
        return 3;
    }

    const std::size_t calls_before_style = measurer->calls;
    TextStyle larger = style;
    larger.font_size += 1.0f;
    cache.measure("a", larger, constraints);
    if (!expect(measurer->calls == calls_before_style + 1, "style differentiates key")) {
        return 4;
    }

    const std::size_t calls_before_constraints = measurer->calls;
    TextConstraints wider = constraints;
    wider.max_width = 200.0f;
    cache.measure("a", style, wider);
    if (!expect(measurer->calls == calls_before_constraints + 1, "constraints differentiate key")) {
        return 5;
    }

    TextMeasureCache invalidating(measurer, 4);
    invalidating.measure("alpha", style, constraints);
    invalidating.measure("beta", style, constraints);
    if (!expect(invalidating.invalidate("alpha") == 1, "invalidate count") ||
        !expect(invalidating.size() == 1, "invalidate shrinks") ||
        !expect(invalidating.invalidate("missing") == 0, "invalidate miss") ||
        !expect(invalidating.stats().invalidations == 1, "invalidation stat")) {
        return 6;
    }

    const std::size_t calls_before_refetch = measurer->calls;
    invalidating.measure("alpha", style, constraints);
    if (!expect(measurer->calls == calls_before_refetch + 1, "invalidated refetch")) {
        return 7;
    }

    invalidating.clear();
    if (!expect(invalidating.size() == 0, "clear size") ||
        !expect(invalidating.stats().hits == 0 && invalidating.stats().misses == 0 &&
                    invalidating.stats().evictions == 0 && invalidating.stats().invalidations == 0,
                "clear stats") ||
        !expect(invalidating.capacity() == 4, "clear capacity")) {
        return 8;
    }

    bool threw = false;
    try {
        TextMeasureCache invalid(measurer, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero capacity rejected")) {
        return 9;
    }

    threw = false;
    try {
        TextMeasureCache invalid(nullptr, 4);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "null measurer rejected")) {
        return 10;
    }

    std::cout << "text_measure_cache: ok\n";
    return 0;
}
