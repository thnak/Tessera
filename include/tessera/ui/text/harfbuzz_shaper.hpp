// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <memory>

namespace tessera::ui::text {

// HarfBuzz maps a UTF-8 run to glyph ids, clusters, and advances. Positions are reported in
// font units by HarfBuzz and scaled to pixels here, so the shaper never mutates its font.
class HarfBuzzShaper final : public TextShaper {
public:
    explicit HarfBuzzShaper(FontFace face);
    ~HarfBuzzShaper() override;

    HarfBuzzShaper(HarfBuzzShaper&&) noexcept;
    HarfBuzzShaper& operator=(HarfBuzzShaper&&) noexcept;
    HarfBuzzShaper(const HarfBuzzShaper&) = delete;
    HarfBuzzShaper& operator=(const HarfBuzzShaper&) = delete;

    [[nodiscard]] ShapedRun shape(std::string_view utf8, const TextStyle& style) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::text
