// SPDX-License-Identifier: MIT

#include "tessera/ui/text/fallback_shaper.hpp"

#include <algorithm>
#include <utility>

#include <hb.h>

namespace tessera::ui::text {
namespace {

constexpr std::size_t kMaxDecisions = 4096;
constexpr std::uint16_t kNoFace = 0xFFFF;

struct Decoded {
    char32_t codepoint = 0;
    std::size_t length = 1;
};

// Tolerant UTF-8 decode: a malformed byte is U+FFFD of length 1, so segmentation never stalls.
Decoded decode(std::string_view text, std::size_t at) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[at + i]); };
    const unsigned char lead = byte(0);
    std::size_t length = 1;
    char32_t cp = lead;
    if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        cp = lead & 0x07U;
    } else if (lead >= 0xE0) {
        length = 3;
        cp = lead & 0x0FU;
    } else if (lead >= 0xC2) {
        length = 2;
        cp = lead & 0x1FU;
    } else if (lead >= 0x80) {
        return {0xFFFD, 1};
    }
    if (at + length > text.size()) {
        return {0xFFFD, 1};
    }
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(i) & 0xC0U) != 0x80U) {
            return {0xFFFD, 1};
        }
        cp = (cp << 6) | (byte(i) & 0x3FU);
    }
    return {cp, length};
}

bool is_regional_indicator(char32_t cp) noexcept {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

// Code points that continue the previous cluster instead of starting a new one.
bool is_extender(char32_t cp) noexcept {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0591 && cp <= 0x05BD) || cp == 0x05BF ||
           cp == 0x05C1 || cp == 0x05C2 || cp == 0x05C4 || cp == 0x05C5 || cp == 0x05C7 ||
           (cp >= 0x064B && cp <= 0x065F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) || // incl. keycap
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0x200D ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

// Invisible code points a face does not need a glyph for: they never decide coverage.
bool is_default_ignorable(char32_t cp) noexcept {
    return (cp >= 0x200B && cp <= 0x200F) || cp == 0x2060 || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xE0020 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

} // namespace

FallbackShaper::FallbackShaper(std::vector<FontFace> stack) {
    for (FontFace& face : stack) {
        if (face.valid()) {
            shapers_.push_back(std::make_unique<HarfBuzzShaper>(std::move(face)));
        }
    }
}

FallbackShaper::~FallbackShaper() = default;
FallbackShaper::FallbackShaper(FallbackShaper&&) noexcept = default;
FallbackShaper& FallbackShaper::operator=(FallbackShaper&&) noexcept = default;

std::size_t FallbackShaper::face_for_cluster(std::string_view cluster) const {
    if (shapers_.empty()) {
        return 0;
    }
    if (const auto found = decisions_.find(std::string(cluster)); found != decisions_.end()) {
        return found->second == kNoFace ? shapers_.size() : found->second;
    }

    std::vector<char32_t> codepoints;
    for (std::size_t at = 0; at < cluster.size();) {
        const Decoded d = decode(cluster, at);
        codepoints.push_back(d.codepoint);
        at += d.length;
    }
    std::uint16_t chosen = kNoFace;
    // First the face that covers the whole cluster, so a sequence is not split across faces.
    for (std::size_t i = 0; i < shapers_.size() && chosen == kNoFace; ++i) {
        const bool covers = std::all_of(codepoints.begin(), codepoints.end(), [&](char32_t cp) {
            return is_default_ignorable(cp) || shapers_[i]->has_glyph(cp);
        });
        if (covers) {
            chosen = static_cast<std::uint16_t>(i);
        }
    }
    // Otherwise the first face that draws the base character.
    for (std::size_t i = 0; i < shapers_.size() && chosen == kNoFace && !codepoints.empty(); ++i) {
        if (shapers_[i]->has_glyph(codepoints.front())) {
            chosen = static_cast<std::uint16_t>(i);
        }
    }
    if (decisions_.size() >= kMaxDecisions) {
        decisions_.clear();
    }
    decisions_.emplace(std::string(cluster), chosen);
    return chosen == kNoFace ? shapers_.size() : chosen;
}

ShapedRun FallbackShaper::shape(std::string_view utf8, const TextStyle& style) const {
    ShapedRun result;
    if (shapers_.empty() || utf8.empty() || style.font_size <= 0.0f) {
        return result;
    }

    // The direction of the whole text, so neutral runs follow their paragraph.
    hb_buffer_t* probe = hb_buffer_create();
    hb_buffer_add_utf8(probe, utf8.data(), static_cast<int>(utf8.size()), 0,
                       static_cast<int>(utf8.size()));
    hb_buffer_guess_segment_properties(probe);
    const bool rtl = hb_buffer_get_direction(probe) == HB_DIRECTION_RTL;
    hb_buffer_destroy(probe);
    result.right_to_left = rtl;

    struct Piece {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t face = 0;
    };
    std::vector<Piece> pieces;

    std::size_t at = 0;
    while (at < utf8.size()) {
        const std::size_t begin = at;
        Decoded d = decode(utf8, at);
        at += d.length;
        std::size_t regional = is_regional_indicator(d.codepoint) ? 1 : 0;
        bool joined = d.codepoint == 0x200D;
        while (at < utf8.size()) {
            const Decoded next = decode(utf8, at);
            const bool pair = regional == 1 && is_regional_indicator(next.codepoint);
            if (!(joined || is_extender(next.codepoint) || pair)) {
                break;
            }
            joined = next.codepoint == 0x200D;
            regional = pair ? 2 : regional;
            at += next.length;
        }
        std::size_t face = face_for_cluster(utf8.substr(begin, at - begin));
        if (face >= shapers_.size()) {
            face = 0; // uncovered: the first face draws .notdef
        }
        if (!pieces.empty() && pieces.back().face == face && pieces.back().end == begin) {
            pieces.back().end = at;
        } else {
            pieces.push_back({begin, at, face});
        }
    }

    std::vector<ShapedRun> shaped;
    shaped.reserve(pieces.size());
    for (const Piece& piece : pieces) {
        ShapedRun sub = shapers_[piece.face]->shape_directed(
            utf8.substr(piece.begin, piece.end - piece.begin), style, rtl);
        for (Glyph& glyph : sub.glyphs) {
            glyph.cluster += static_cast<std::uint32_t>(piece.begin);
            glyph.face = static_cast<std::uint16_t>(piece.face);
        }
        shaped.push_back(std::move(sub));
    }
    if (rtl) {
        std::reverse(shaped.begin(), shaped.end()); // visual order
    }
    for (ShapedRun& sub : shaped) {
        result.advance += sub.advance;
        result.ascent = std::max(result.ascent, sub.ascent);
        result.descent = std::max(result.descent, sub.descent);
        result.missing_glyphs += sub.missing_glyphs;
        result.glyphs.insert(result.glyphs.end(), sub.glyphs.begin(), sub.glyphs.end());
    }
    return result;
}

} // namespace tessera::ui::text
