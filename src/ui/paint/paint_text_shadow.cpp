// SPDX-License-Identifier: MIT
//
// CSS `text-shadow` (issue #616): the list grammar, the paint-side reach, and the sink wrapper that
// paints each run's shadows behind its glyphs. The blur is a bounded offset-copy stack, the same
// kind of approximation `box-shadow` makes with its concentric fills; see
// docs/design/text-shadow.md for why, and for the cost bound.

#include "paint_text_shadow.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/color.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::paint::detail {

using tessera::detail::lower;
using tessera::detail::split_top_level;
using tessera::detail::trim;
namespace {

// Unit offsets of the copies placed on one ring, at 45 degree steps. A table instead of
// sin/cos keeps the paint path free of trigonometry and makes the emitted geometry exactly
// reproducible across platforms (a golden compares these positions).
struct UnitOffset {
    float x;
    float y;
};
constexpr UnitOffset kRingOffsets[kTextShadowRingSamples] = {
    {1.0f, 0.0f},  {0.70710678f, 0.70710678f},   {0.0f, 1.0f},  {-0.70710678f, 0.70710678f},
    {-1.0f, 0.0f}, {-0.70710678f, -0.70710678f}, {0.0f, -1.0f}, {0.70710678f, -0.70710678f},
};

bool parse_px(std::string_view text, float& value) {
    std::string token = lower(trim(text));
    bool has_unit = false;
    if (token.size() > 2 && token.substr(token.size() - 2) == "px") {
        token.resize(token.size() - 2);
        has_unit = true;
    } else if (!token.empty() && std::isalpha(static_cast<unsigned char>(token.back())) != 0) {
        return false;
    }
    if (token.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(token.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    // A unit-less number is only a length when it is 0.
    if (!has_unit && parsed != 0.0f) {
        return false;
    }
    value = parsed;
    return true;
}

bool is_length_token(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    const char c = token.front();
    return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '+' || c == '.';
}

// Splits at `separator` outside parentheses (a colour such as rgba(0, 0, 0, .2) has commas and
// spaces of its own).

// `[<color>? <x> <y> <blur>?]`. Unlike `box-shadow` there is no spread and no `inset` keyword
// (CSS Text Decoration 3 section 3), so a fourth length or an `inset` token is invalid rather than
// silently ignored.
bool parse_one(std::string_view text, const rhi::Color& current_color, TextShadow& out) {
    float lengths[3] = {};
    int length_count = 0;
    bool have_color = false;
    out = TextShadow{};
    out.color = current_color;
    for (const std::string_view token : split_top_level(text, ' ')) {
        if (lower(token) == "inset") {
            return false; // text-shadow has no inset form
        }
        if (is_length_token(token)) {
            if (length_count == 3 || !parse_px(token, lengths[length_count])) {
                return false;
            }
            ++length_count;
        } else if (lower(token) == "currentcolor") {
            if (have_color) {
                return false;
            }
            have_color = true;
            out.color = current_color;
        } else {
            const style::ColorParseResult parsed = style::parse_color(token);
            if (!parsed.ok || have_color) {
                return false;
            }
            have_color = true;
            out.color = rhi::Color{parsed.color.r, parsed.color.g, parsed.color.b, parsed.color.a};
        }
    }
    if (length_count < 2) {
        return false;
    }
    out.offset_x = lengths[0];
    out.offset_y = lengths[1];
    out.blur = length_count >= 3 ? lengths[2] : 0.0f;
    return out.blur >= 0.0f; // a negative blur radius is invalid
}

// Standard normal CDF: the Gaussian coverage of a blurred edge at signed distance `x` outward.
float normal_cdf(float x) {
    return 0.5f * std::erfc(-x / std::sqrt(2.0f));
}

// Emits one shadow as offset copies of `run`. A sharp shadow is one copy. A blurred one is a stack
// of rings: ring j sits at radius r_j and carries the alpha that lifts the cumulative composite to
// the Gaussian coverage at that radius, and a final centred copy carries the full colour so the
// glyph interior is solid. The rings are the text analogue of `box-shadow`'s concentric fills
// (paint_shadow.cpp): the same sigma = blur / 2, the same cumulative-alpha recurrence, and the same
// bounded layer count, so the two effects fade alike and neither can flood a batch.
void emit_shadow(PaintSink& sink, const TextDraw& run, const TextShadow& shadow, float opacity,
                 bool reduce_effects, PaintStats& stats) {
    const float alpha = shadow.color.a * opacity;
    if (alpha <= 0.0f) {
        return;
    }
    rhi::Color color = shadow.color;
    color.a = alpha;

    const auto emit_copy = [&](float dx, float dy, float copy_alpha) {
        if (copy_alpha <= 0.0f) {
            return;
        }
        TextDraw copy = run;
        copy.rect.x += dx;
        copy.rect.y += dy;
        if (copy.baseline.has_value()) {
            copy.baseline = *copy.baseline + dy;
        }
        copy.color = color;
        copy.color.a = copy_alpha;
        // A shadow is a blurred or offset impression of the glyphs: per-channel LCD coverage would
        // put colour fringes on it, so it always composites grayscale.
        copy.lcd = false;
        sink.draw_text(copy);
        ++stats.text_shadows;
    };

    if (shadow.blur <= 0.0f) {
        emit_copy(shadow.offset_x, shadow.offset_y, alpha);
        return;
    }

    const float sigma = shadow.blur * 0.5f;
    const int full_rings = std::clamp(static_cast<int>(std::ceil(shadow.blur * 0.5f)),
                                      kMinTextShadowRings, kMaxTextShadowRings);
    const int rings = reduce_effects ? std::min(full_rings, kReducedTextShadowRings) : full_rings;
    // Rings from the outer reach inward; the last band is the centred copy at full alpha.
    const int ring_count = std::max(rings - 1, 1);
    float previous = 0.0f;
    for (int j = 0; j < ring_count; ++j) {
        const float radius =
            shadow.blur * static_cast<float>(ring_count - j) / static_cast<float>(ring_count);
        const float target = alpha * normal_cdf(-radius / sigma);
        const float ring_alpha =
            previous >= 1.0f ? 0.0f : 1.0f - (1.0f - target) / (1.0f - previous);
        previous = target;
        if (ring_alpha <= 0.0f) {
            continue;
        }
        for (const UnitOffset& offset : kRingOffsets) {
            emit_copy(shadow.offset_x + offset.x * radius, shadow.offset_y + offset.y * radius,
                      ring_alpha);
        }
    }
    emit_copy(shadow.offset_x, shadow.offset_y, alpha);
}

} // namespace

TextShadowList parse_text_shadows(const style::ComputedStyle& style,
                                  const rhi::Color& current_color) {
    TextShadowList list;
    const std::string* value = style.find("text-shadow");
    if (value == nullptr) {
        return list;
    }
    const std::string_view text = trim(*value);
    if (text.empty() || lower(text) == "none") {
        return list;
    }
    for (const std::string_view entry : split_top_level(text, ',')) {
        TextShadow shadow;
        if (!parse_one(trim(entry), current_color, shadow)) {
            return TextShadowList{};
        }
        if (list.count < kMaxTextShadows) {
            list.items[list.count++] = shadow;
        }
    }
    return list;
}

TextShadowOutsets text_shadow_outsets(const style::ComputedStyle& style) {
    TextShadowOutsets outsets;
    // Reach does not depend on the colour, so any colour works for the parse.
    const TextShadowList list = parse_text_shadows(style, rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
    for (std::size_t i = 0; i < list.count; ++i) {
        const TextShadow& shadow = list.items[i];
        outsets.blurred = outsets.blurred || shadow.blur > 0.0f;
        outsets.left = std::max(outsets.left, shadow.blur - shadow.offset_x);
        outsets.right = std::max(outsets.right, shadow.blur + shadow.offset_x);
        outsets.top = std::max(outsets.top, shadow.blur - shadow.offset_y);
        outsets.bottom = std::max(outsets.bottom, shadow.blur + shadow.offset_y);
    }
    return outsets;
}

void TextShadowSink::draw_text(const TextDraw& run) {
    // Consecutive runs are one visual text block (the lines of a wrapped paragraph, the slices of a
    // span-attributed line), so their shadows are held until the block ends and then emitted
    // together. That is what keeps every shadow behind every glyph of the block: emitting each
    // run's shadow immediately would let the next run's shadow darken the previous run's glyphs.
    pending_.push_back(run);
}

void TextShadowSink::flush() {
    if (pending_.empty()) {
        return;
    }
    // CSS paints the first shadow on top, so emit the list back to front.
    for (std::size_t index = shadows_.count; index-- > 0;) {
        const TextShadow& shadow = shadows_.items[index];
        for (const TextDraw& run : pending_) {
            if (!run.text.empty()) {
                emit_shadow(*target_, run, shadow, opacity_, reduce_effects_, *stats_);
            }
        }
    }
    for (const TextDraw& run : pending_) {
        target_->draw_text(run);
    }
    pending_.clear();
}

} // namespace tessera::ui::paint::detail
