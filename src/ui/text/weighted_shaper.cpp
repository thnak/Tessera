// SPDX-License-Identifier: MIT

#include "tessera/ui/text/weighted_shaper.hpp"

#include <utility>

namespace tessera::ui::text {
namespace {

// The first index whose weight is the closest one inside [low, high] in the given direction:
// ascending picks the smallest weight >= from, descending the largest weight <= from.
std::size_t closest(std::span<const int> weights, int from, bool ascending, bool& found) noexcept {
    std::size_t best = 0;
    found = false;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        const int w = weights[i];
        if (ascending ? w < from : w > from) {
            continue;
        }
        if (!found || (ascending ? w < weights[best] : w > weights[best])) {
            best = i;
            found = true;
        }
    }
    return best;
}

} // namespace

std::size_t match_font_weight(std::span<const int> weights, int requested) noexcept {
    if (weights.empty()) {
        return 0;
    }
    const int want = requested > 0 ? requested : 400;
    bool found = false;
    std::size_t index = closest(weights, want, true, found);
    if (found && weights[index] == want) {
        return index;
    }
    if (want >= 400 && want <= 500) {
        // Heavier faces up to 500 first, then lighter, then everything heavier than 500.
        std::size_t up = closest(weights, want, true, found);
        if (found && weights[up] <= 500) {
            return up;
        }
        const std::size_t down = closest(weights, want, false, found);
        if (found) {
            return down;
        }
        return closest(weights, want, true, found);
    }
    if (want < 400) {
        const std::size_t down = closest(weights, want, false, found);
        if (found) {
            return down;
        }
        return closest(weights, want, true, found);
    }
    const std::size_t up = closest(weights, want, true, found);
    if (found) {
        return up;
    }
    return closest(weights, want, false, found);
}

WeightedShaper::WeightedShaper(std::vector<FontFace> faces) {
    for (FontFace& face : faces) {
        if (face.valid()) {
            weights_.push_back(face.weight());
            shapers_.push_back(std::make_unique<HarfBuzzShaper>(std::move(face)));
        }
    }
}

WeightedShaper::~WeightedShaper() = default;
WeightedShaper::WeightedShaper(WeightedShaper&&) noexcept = default;
WeightedShaper& WeightedShaper::operator=(WeightedShaper&&) noexcept = default;

std::size_t WeightedShaper::index_for(int weight) const noexcept {
    return match_font_weight(weights_, weight);
}

ShapedRun WeightedShaper::shape(std::string_view utf8, const TextStyle& style) const {
    if (shapers_.empty()) {
        return {};
    }
    const std::size_t index = index_for(style.font_weight);
    ShapedRun run = shapers_[index]->shape(utf8, style);
    if (index != 0) {
        for (Glyph& glyph : run.glyphs) {
            glyph.face = static_cast<std::uint16_t>(index);
        }
    }
    return run;
}

} // namespace tessera::ui::text
