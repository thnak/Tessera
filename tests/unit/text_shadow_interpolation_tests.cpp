// SPDX-License-Identifier: MIT
//
// `text-shadow` interpolation (issue #616): the list animates layer by layer like `box-shadow`,
// but its grammar has no spread and no `inset`, so a value carrying either is not a `text-shadow`
// and must not interpolate. The emitted value must also stay parseable by the paint pass, which is
// what the round-trip check below pins.

#include "../../src/ui/animation/shadow_interpolation.hpp"
#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/keyframes.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using tessera::ui::animation::interpolate_text_shadow;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_shadow_interpolation: " << message << "\n";
    }
    return condition;
}

// A `text-shadow` layer read back through the same colour parser the paint pass uses. `lengths`
// is 2 or 3: a spread would be a fourth, which the grammar does not allow.
struct Parsed {
    float x = 0.0f;
    float y = 0.0f;
    float blur = 0.0f;
    int lengths = 0;
    tessera::ui::style::ColorRgba color;
    bool ok = false;
};

Parsed read_one(const std::string& text) {
    Parsed out;
    const char* cursor = text.c_str();
    float values[4] = {};
    int count = 0;
    while (count < 4) {
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor || end[0] != 'p' || end[1] != 'x') {
            break;
        }
        values[count++] = value;
        cursor = end + 2;
    }
    if (count < 2) {
        return out;
    }
    const std::size_t consumed = static_cast<std::size_t>(cursor - text.c_str());
    const auto color = tessera::ui::style::parse_color(text.substr(consumed));
    out.x = values[0];
    out.y = values[1];
    out.blur = count >= 3 ? values[2] : 0.0f;
    out.lengths = count;
    out.color = color.color;
    out.ok = color.ok;
    return out;
}

} // namespace

int main() {
    // Two single layers: offsets, blur and colour all interpolate.
    const auto mid =
        interpolate_text_shadow("0 0 0 rgba(0, 0, 0, 0)", "0 8px 16px rgba(0, 0, 0, 0.4)", 0.5);
    if (!expect(mid.has_value(), "two single layers interpolate")) {
        return 1;
    }
    const Parsed m = read_one(*mid);
    if (!expect(m.ok && m.lengths == 3, "the result is a three-length text-shadow")) {
        std::cerr << "got: " << *mid << "\n";
        return 1;
    }
    if (!expect(std::fabs(m.y - 4.0f) < 0.01f && std::fabs(m.blur - 8.0f) < 0.01f,
                "offsets and blur interpolate")) {
        std::cerr << "got: " << *mid << "\n";
        return 1;
    }
    if (!expect(std::fabs(m.color.a - 0.2f) < 0.01f, "the colour interpolates")) {
        std::cerr << "got: " << *mid << "\n";
        return 1;
    }

    // `none` fades a shadow in instead of snapping: the missing layer is a transparent zero shadow.
    const auto fade = interpolate_text_shadow("none", "0 4px 8px rgba(0, 0, 0, 0.5)", 0.25);
    if (!expect(fade.has_value(), "none interpolates")) {
        return 2;
    }
    const Parsed f = read_one(*fade);
    if (!expect(f.ok && std::fabs(f.y - 1.0f) < 0.01f && std::fabs(f.blur - 2.0f) < 0.01f &&
                    std::fabs(f.color.a - 0.125f) < 0.01f,
                "a quarter of the way is a quarter of the shadow")) {
        std::cerr << "got: " << *fade << "\n";
        return 2;
    }

    // A list interpolates layer by layer, and the shorter list is padded.
    const auto list =
        interpolate_text_shadow("1px 1px 2px #000, 2px 2px 4px #000", "3px 3px 6px #000", 0.5);
    if (!expect(list.has_value(), "a list interpolates against a shorter list")) {
        return 3;
    }
    if (!expect(list->find(", ") != std::string::npos, "the result keeps both layers")) {
        std::cerr << "got: " << *list << "\n";
        return 3;
    }

    // The grammar has no spread and no inset: a value carrying either is not a `text-shadow`.
    if (!expect(!interpolate_text_shadow("0 0 4px 2px #000", "0 0 8px 2px #000", 0.5).has_value(),
                "a fourth length (spread) does not interpolate")) {
        return 4;
    }
    if (!expect(!interpolate_text_shadow("inset 0 0 4px #000", "0 0 8px #000", 0.5).has_value(),
                "inset does not interpolate")) {
        return 4;
    }
    if (!expect(!interpolate_text_shadow("garbage", "0 0 4px #000", 0.5).has_value(),
                "an unparseable value does not interpolate")) {
        return 4;
    }

    // Through the keyframe sampler (a transition is two keyframes): the property is no longer a
    // 50 % switch.
    tessera::ui::style::KeyframeBlock block;
    tessera::ui::style::KeyframeStep from;
    from.offset = 0.0;
    from.declarations.push_back({"text-shadow", "none", false});
    tessera::ui::style::KeyframeStep to;
    to.offset = 100.0;
    to.declarations.push_back({"text-shadow", "0 4px 8px rgba(0, 0, 0, 0.4)", false});
    block.steps.push_back(from);
    block.steps.push_back(to);
    tessera::ui::animation::KeyframeTiming timing;
    timing.duration = 1.0;
    const auto sampled = tessera::ui::animation::sample_keyframes(block, timing, 0.25);
    if (!expect(sampled.size() == 1 && sampled[0].property == "text-shadow",
                "one text-shadow declaration")) {
        return 5;
    }
    const Parsed s = read_one(sampled[0].value);
    if (!expect(s.ok && s.y > 0.0f && s.y < 4.0f && s.color.a > 0.0f && s.color.a < 0.4f,
                "a quarter of the way is neither end")) {
        std::cerr << "got: " << sampled[0].value << "\n";
        return 5;
    }
    return 0;
}
