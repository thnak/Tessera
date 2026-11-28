// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::ui::text {

// One OpenType feature request: a 4-byte tag packed big-endian ("tnum" -> 0x746E756D) and the
// feature value (0 = off, 1 = on, >1 selects an alternate for features that take one).
struct FontFeature {
    std::uint32_t tag = 0;
    std::uint32_t value = 0;

    friend constexpr bool operator==(const FontFeature&, const FontFeature&) = default;
    friend constexpr auto operator<=>(const FontFeature&, const FontFeature&) = default;
};

[[nodiscard]] constexpr std::uint32_t font_feature_tag(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

// The OpenType features a text style asks the shaper to apply (docs/design/text.md section 3.1).
// A fixed-capacity value so a TextStyle stays trivially copyable (no hidden allocation on the
// measure and shape paths) and every cache key can compare and order it. Unused slots stay zero,
// so the defaulted comparisons are exact. A request past kMax is dropped silently.
struct FontFeatures {
    // Room for a worst-case variant set (five ligature, four numeric, two caps, three east-asian,
    // one position feature), the alternates functions and a handful of raw font-feature-settings.
    static constexpr std::size_t kMax = 32;

    std::array<FontFeature, kMax> items{};
    std::uint8_t count = 0;

    // Sets 'tag' to 'value'; a later request for the same tag replaces the earlier one in place
    // (the more specific declaration wins). False when the list is full.
    bool set(std::uint32_t tag, std::uint32_t value) noexcept;

    [[nodiscard]] bool empty() const noexcept {
        return count == 0;
    }

    friend constexpr bool operator==(const FontFeatures&, const FontFeatures&) = default;
    friend constexpr auto operator<=>(const FontFeatures&, const FontFeatures&) = default;
};

// One feature request that covers only a byte range of the shaped text (the per-range hb_feature_t
// start/end of issue #1011), for rich-text spans: `<span style="font-variant-numeric:
// tabular-nums">` inside a paragraph. Offsets are UTF-8 bytes of the string handed to the shaper;
// begin >= end covers nothing. HarfBuzz widens a range to the clusters it touches.
struct FontFeatureRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    FontFeature feature;

    friend constexpr bool operator==(const FontFeatureRange&, const FontFeatureRange&) = default;
};

// Most ranges one shaping call honours; later ones are dropped (bounded stack storage, no
// allocation on the shape path).
inline constexpr std::size_t kMaxFontFeatureRanges = 32;

// A process-wide handle to an immutable FontFeatures list, so a TextStyle, a TextDraw and a
// recorded paint op carry 4 bytes instead of the list (sizeof(PaintOp) is budgeted, issue #731). 0
// means "no features". Equal lists share one id, so comparing and hashing ids is exact. The table
// holds at most kMaxInterned distinct lists; past that a new list interns as 0 (features dropped)
// rather than growing without bound. Interning is thread-safe and takes a lock only for a non-empty
// list.
struct FontFeaturesId {
    std::uint32_t value = 0;

    friend constexpr bool operator==(FontFeaturesId, FontFeaturesId) = default;
    friend constexpr auto operator<=>(FontFeaturesId, FontFeaturesId) = default;
};

inline constexpr std::size_t kMaxInternedFontFeatures = 256;

[[nodiscard]] FontFeaturesId intern_font_features(const FontFeatures& features);
// The list behind an id; an unknown id (including 0) resolves to the empty list.
[[nodiscard]] FontFeatures resolve_font_features(FontFeaturesId id);

// How many times the intern table was full and a new list fell back to "no features". A non-zero
// value means a page uses more than kMaxInternedFontFeatures distinct feature sets (issue #998).
[[nodiscard]] std::uint64_t font_features_intern_dropped() noexcept;

// The computed font-variant-* values and font-feature-settings of one text style. Each view is the
// computed value text, or empty when the property is not set.
struct FontVariantValues {
    std::string_view ligatures;
    std::string_view numeric;
    std::string_view caps;
    std::string_view east_asian;
    std::string_view position;
    std::string_view alternates;
    std::string_view settings;
    // True when the style's letter-spacing is not zero: optional ligatures (liga, clig, dlig, hlig)
    // start off then (CSS Text 3 section 7.3), and an explicit variant or feature setting below
    // still turns them back on.
    bool letter_spaced = false;
};

// Combined features in CSS order: the letter-spacing ligature default, then ligatures, numeric,
// caps, east-asian, position, then font-feature-settings, so a raw feature setting overrides the
// matching variant keyword (CSS Fonts 4 section 6.12). An invalid value is ignored as a whole (the
// CSS rule for an invalid declaration), never partly applied.
[[nodiscard]] FontFeatures parse_font_features(const FontVariantValues& values);

// The parsers, exposed for tests. Each appends to 'out' and returns false (leaving 'out'
// unchanged) when the value is invalid; 'normal' is valid and adds nothing.
bool parse_font_variant_ligatures(std::string_view value, FontFeatures& out);
bool parse_font_variant_numeric(std::string_view value, FontFeatures& out);
bool parse_font_variant_caps(std::string_view value, FontFeatures& out);
bool parse_font_variant_east_asian(std::string_view value, FontFeatures& out);
bool parse_font_variant_position(std::string_view value, FontFeatures& out);
bool parse_font_variant_alternates(std::string_view value, FontFeatures& out);
bool parse_font_feature_settings(std::string_view value, FontFeatures& out);

} // namespace tessera::ui::text
