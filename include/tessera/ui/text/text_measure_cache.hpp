// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace tessera::ui::text {

class TextMeasureCache {
public:
    struct Stats {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t evictions = 0;
        std::size_t invalidations = 0;
        std::size_t entries = 0;
    };

    explicit TextMeasureCache(std::shared_ptr<const TextMeasurer> measurer,
                              std::size_t capacity = 1024);
    ~TextMeasureCache();

    TextMeasureCache(TextMeasureCache&&) noexcept;
    TextMeasureCache& operator=(TextMeasureCache&&) noexcept;
    TextMeasureCache(const TextMeasureCache&) = delete;
    TextMeasureCache& operator=(const TextMeasureCache&) = delete;

    TextMetrics measure(std::string_view text, const TextStyle& style,
                        const TextConstraints& constraints);

    std::size_t invalidate(std::string_view text);
    void clear();

    // Bumped by every `invalidate` and `clear`, whether or not an entry was removed. A holder of a
    // result derived from `measure` (the layout engine's per-node text-fit fact) compares it to
    // know the result may no longer match what `measure` would return now. Eviction does not bump
    // it: it only forgets a value the measurer would produce again unchanged.
    [[nodiscard]] std::uint64_t generation() const;

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const;
    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::text
