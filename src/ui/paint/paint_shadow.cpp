// SPDX-License-Identifier: MIT

#include "paint_shadow.hpp"
#include "tessera/ui/style/color.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::paint::detail {
namespace {

// Concentric fills per blurred shadow. More layers approximate the Gaussian edge more smoothly; the
// cap keeps a large blur from flooding the batch.
constexpr int kMinBlurLayers = 3;
constexpr int kMaxBlurLayers = 8;

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

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

// Splits at `separator` outside parentheses.
std::vector<std::string_view> split_top_level(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size()) {
            if (text[i] == '(') {
                ++depth;
            } else if (text[i] == ')') {
                --depth;
            }
        }
        const bool at_separator =
            i < text.size() &&
            (separator == ' ' ? std::isspace(static_cast<unsigned char>(text[i])) != 0
                              : text[i] == separator) &&
            depth == 0;
        if (i == text.size() || at_separator) {
            if (i > start) {
                parts.push_back(text.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return parts;
}

bool parse_one(std::string_view text, const rhi::Color& current_color, BoxShadow& out) {
    float lengths[4] = {};
    int length_count = 0;
    bool have_color = false;
    out = BoxShadow{};
    out.color = current_color;
    for (const std::string_view token : split_top_level(text, ' ')) {
        if (lower(token) == "inset") {
            out.inset = true;
        } else if (is_length_token(token)) {
            if (length_count == 4 || !parse_px(token, lengths[length_count])) {
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
    out.spread = length_count >= 4 ? lengths[3] : 0.0f;
    return out.blur >= 0.0f; // a negative blur radius is invalid
}

// Standard normal CDF.
float normal_cdf(float x) {
    return 0.5f * std::erfc(-x / std::sqrt(2.0f));
}

} // namespace

BoxShadowList parse_box_shadows(const style::ComputedStyle& style,
                                const rhi::Color& current_color) {
    BoxShadowList list;
    const std::string* value = style.find("box-shadow");
    if (value == nullptr) {
        return list;
    }
    const std::string_view text = trim(*value);
    if (text.empty() || lower(text) == "none") {
        return list;
    }
    for (const std::string_view entry : split_top_level(text, ',')) {
        BoxShadow shadow;
        if (!parse_one(trim(entry), current_color, shadow)) {
            return BoxShadowList{};
        }
        if (list.count < kMaxBoxShadows) {
            list.items[list.count++] = shadow;
        }
    }
    return list;
}

ShadowOutsets shadow_outsets(const style::ComputedStyle& style) {
    ShadowOutsets outsets;
    // Reach does not depend on the color, so any color works for the parse.
    const BoxShadowList list = parse_box_shadows(style, rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
    for (std::size_t i = 0; i < list.count; ++i) {
        const BoxShadow& shadow = list.items[i];
        if (shadow.inset) {
            continue;
        }
        const float reach = std::max(0.0f, shadow.spread + shadow.blur);
        outsets.left = std::max(outsets.left, reach - shadow.offset_x);
        outsets.right = std::max(outsets.right, reach + shadow.offset_x);
        outsets.top = std::max(outsets.top, reach - shadow.offset_y);
        outsets.bottom = std::max(outsets.bottom, reach + shadow.offset_y);
    }
    return outsets;
}

void paint_box_shadows(const style::ComputedStyle& style, const rhi::Rect& rect, float radius,
                       float opacity, const rhi::Color& current_color, PaintSink& sink,
                       PaintStats& stats, bool reduce_effects) {
    if (opacity <= 0.0f) {
        return;
    }
    const BoxShadowList list = parse_box_shadows(style, current_color);
    // CSS paints the first shadow on top, so emit the list back to front.
    for (std::size_t index = list.count; index-- > 0;) {
        const BoxShadow& shadow = list.items[index];
        const float alpha = shadow.color.a * opacity;
        if (shadow.inset || alpha <= 0.0f) {
            continue;
        }
        const auto emit = [&](float expansion, float layer_alpha, float corner) {
            const rhi::Rect box{rect.x + shadow.offset_x - expansion,
                                rect.y + shadow.offset_y - expansion, rect.width + 2.0f * expansion,
                                rect.height + 2.0f * expansion};
            if (box.width <= 0.0f || box.height <= 0.0f || layer_alpha <= 0.0f) {
                return;
            }
            rhi::Color color = shadow.color;
            color.a = layer_alpha;
            const float clamped = std::clamp(corner, 0.0f, std::min(box.width, box.height) * 0.5f);
            if (clamped > 0.0f) {
                sink.fill_rounded_rect(box, color, clamped);
            } else {
                sink.fill_rect(box, color);
            }
            ++stats.shadows;
        };

        if (shadow.blur <= 0.0f) {
            emit(shadow.spread, alpha, radius > 0.0f ? radius + shadow.spread : 0.0f);
            continue;
        }

        // Gaussian edge: sigma = blur / 2, so the edge fades over [-blur, +blur] around the
        // (spread) shadow edge. Layer j sits at signed distance x_j outward from that edge; the
        // alpha of each layer is chosen so the cumulative "over" composite inside layer j equals
        // the Gaussian coverage at the middle of its band. The innermost band is the full color.
        const float sigma = shadow.blur * 0.5f;
        const int full_layers = std::clamp(static_cast<int>(std::ceil(shadow.blur * 0.5f)),
                                           kMinBlurLayers, kMaxBlurLayers);
        const int layers = reduce_effects ? std::min(full_layers, kReducedBlurLayers) : full_layers;
        const float step = 2.0f * shadow.blur / static_cast<float>(layers);
        float previous = 0.0f;
        for (int j = 0; j < layers; ++j) {
            const float x_outer = shadow.blur - static_cast<float>(j) * step;
            const float x_inner = x_outer - step;
            const float target =
                j == layers - 1 ? alpha : alpha * normal_cdf(-0.5f * (x_outer + x_inner) / sigma);
            const float layer_alpha =
                previous >= 1.0f ? 0.0f : 1.0f - (1.0f - target) / (1.0f - previous);
            previous = target;
            const float corner =
                radius > 0.0f ? radius + shadow.spread + x_outer : std::max(0.0f, x_outer);
            emit(shadow.spread + x_outer, layer_alpha, corner);
        }
    }
}

} // namespace tessera::ui::paint::detail
