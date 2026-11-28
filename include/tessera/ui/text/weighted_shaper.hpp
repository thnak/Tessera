// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace tessera::ui::text {

// CSS Fonts 4 section 5.2 weight matching over the weights of the loaded faces of one family:
// the index of the face a request for `requested` draws with. An exact weight wins; a request in
// 400..500 tries up to 500 first, then lighter, then heavier; below 400 it tries lighter then
// heavier; above 500 it tries heavier then lighter. The first face wins a tie. `requested <= 0`
// means "unset" and is treated as 400. Returns 0 for an empty set.
[[nodiscard]] std::size_t match_font_weight(std::span<const int> weights, int requested) noexcept;

// Shapes with the face of a family whose weight best matches `TextStyle::font_weight` (a static
// face per weight: no synthetic bold, no variable axes). `Glyph::face` is the index of the chosen
// face in the order the faces were given, so a `TextRenderer` built over rasterizers in the same
// order draws each glyph with the face that shaped it. With one face it behaves exactly like
// `HarfBuzzShaper`. Not thread-safe, like the faces it owns.
class WeightedShaper final : public TextShaper {
public:
    // At least one valid face; invalid faces are dropped and the order of the rest is kept.
    explicit WeightedShaper(std::vector<FontFace> faces);
    ~WeightedShaper() override;
    WeightedShaper(WeightedShaper&&) noexcept;
    WeightedShaper& operator=(WeightedShaper&&) noexcept;
    WeightedShaper(const WeightedShaper&) = delete;
    WeightedShaper& operator=(const WeightedShaper&) = delete;

    [[nodiscard]] ShapedRun shape(std::string_view utf8, const TextStyle& style) const override;

    // The face index `shape` uses for a CSS weight; metrics providers use it to stay in step.
    [[nodiscard]] std::size_t index_for(int weight) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return shapers_.size();
    }

private:
    std::vector<std::unique_ptr<HarfBuzzShaper>> shapers_;
    std::vector<int> weights_;
};

} // namespace tessera::ui::text
