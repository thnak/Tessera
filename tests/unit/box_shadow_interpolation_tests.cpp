// SPDX-License-Identifier: MIT

#include "../../src/ui/animation/shadow_interpolation.hpp"
#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/keyframes.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using tessera::ui::animation::interpolate_box_shadow;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "box_shadow_interpolation: " << message << "\n";
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.01f;
}

// The midpoint of two single-layer shadows, read back through the same colour parser the paint
// pass uses.
struct Parsed {
    float x = 0, y = 0, blur = 0, spread = 0;
    tessera::ui::style::ColorRgba color;
    bool inset = false;
    bool ok = false;
};

Parsed read_one(const std::string& text) {
    Parsed out;
    std::string rest = text;
    if (rest.rfind("inset ", 0) == 0) {
        out.inset = true;
        rest = rest.substr(6);
    }
    float v[4] = {};
    const char* cursor = rest.c_str();
    for (float& value : v) {
        char* end = nullptr;
        value = std::strtof(cursor, &end);
        if (end == cursor || end[0] != 'p' || end[1] != 'x') {
            return out;
        }
        cursor = end + 2;
    }
    const std::size_t consumed = static_cast<std::size_t>(cursor - rest.c_str());
    const auto color = tessera::ui::style::parse_color(rest.substr(consumed));
    out.x = v[0];
    out.y = v[1];
    out.blur = v[2];
    out.spread = v[3];
    out.color = color.color;
    out.ok = color.ok;
    return out;
}

} // namespace

int main() {
    // Two single layers: offsets, blur, spread and colour all interpolate.
    const auto mid = interpolate_box_shadow("0 0 0 0 rgba(0, 0, 0, 0)",
                                            "0 8px 16px 4px rgba(0, 0, 0, 0.4)", 0.5);
    if (!expect(mid.has_value(), "two single-layer shadows interpolate")) {
        return 1;
    }
    const Parsed a = read_one(*mid);
    if (!expect(a.ok && near(a.x, 0) && near(a.y, 4) && near(a.blur, 8) && near(a.spread, 2),
                "midpoint lengths") ||
        !expect(near(a.color.a, 0.2f), "midpoint alpha")) {
        std::cerr << "got: " << *mid << "\n";
        return 2;
    }

    // `none` against a shadow pads with a transparent zero shadow: a fade-in, not a snap.
    const auto fade = interpolate_box_shadow("none", "0 4px 8px 0 rgba(0, 0, 0, 0.5)", 0.25);
    if (!expect(fade.has_value(), "none pairs with a shadow")) {
        return 3;
    }
    const Parsed b = read_one(*fade);
    if (!expect(b.ok && near(b.y, 1) && near(b.blur, 2) && near(b.color.a, 0.125f),
                "none -> shadow at a quarter")) {
        std::cerr << "got: " << *fade << "\n";
        return 3;
    }

    // A shorter list is padded: the second layer of the target fades in while the first moves.
    const auto padded =
        interpolate_box_shadow("0 2px 4px 0 rgba(0, 0, 0, 0.2)",
                               "0 2px 4px 0 rgba(0, 0, 0, 0.2), 0 8px 0 0 rgba(255, 0, 0, 1)", 0.5);
    if (!expect(padded.has_value() && padded->find(", ") != std::string::npos,
                "a longer target keeps its extra layer")) {
        return 4;
    }

    // Layers whose `inset` flags differ do not interpolate; the caller keeps its discrete switch.
    if (!expect(!interpolate_box_shadow("inset 0 0 4px 0 #000", "0 0 4px 0 #000", 0.5).has_value(),
                "inset mismatch is incompatible") ||
        !expect(
            !interpolate_box_shadow("0 0 4px 0 currentColor", "0 0 8px 0 #000", 0.5).has_value(),
            "currentColor is left to the discrete switch") ||
        !expect(!interpolate_box_shadow("garbage", "0 0 4px 0 #000", 0.5).has_value(),
                "an invalid list is incompatible")) {
        return 5;
    }

    // Through the keyframe sampler (a transition is two keyframes): the property is no longer a
    // 50 % switch.
    tessera::ui::style::KeyframeBlock block;
    tessera::ui::style::KeyframeStep from;
    from.offset = 0.0;
    from.declarations.push_back({"box-shadow", "none", false});
    tessera::ui::style::KeyframeStep to;
    to.offset = 100.0;
    to.declarations.push_back({"box-shadow", "0 8px 16px 0 rgba(0, 0, 0, 0.4)", false});
    block.steps.push_back(from);
    block.steps.push_back(to);
    tessera::ui::animation::KeyframeTiming timing;
    timing.duration = 1.0;
    const auto sampled = tessera::ui::animation::sample_keyframes(block, timing, 0.25);
    if (!expect(sampled.size() == 1 && sampled[0].property == "box-shadow", "one declaration")) {
        return 6;
    }
    const Parsed c = read_one(sampled[0].value);
    if (!expect(c.ok && c.y > 0.0f && c.y < 8.0f && c.color.a > 0.0f && c.color.a < 0.4f,
                "a quarter of the way is neither end")) {
        std::cerr << "got: " << sampled[0].value << "\n";
        return 6;
    }
    return 0;
}
