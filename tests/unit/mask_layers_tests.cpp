// SPDX-License-Identifier: MIT
//
// The mask geometry longhands and the `mask` shorthand (issue #617): `mask-size`, `mask-position`,
// `mask-repeat`, `mask-mode`, `mask-origin`, `mask-clip` parse to per-layer values, a short list
// repeats cyclically, an invalid list drops its declaration with a diagnostic, and `mask` expands
// every longhand per layer.

#include "tessera/ui/style/mask_layers.hpp"
#include "tessera/ui/style/property_registry.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace tessera::ui::style;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "mask_layers: " << message << "\n";
    }
    return condition;
}

bool near(float a, float b) {
    return std::abs(a - b) < 1e-4f;
}

bool sizes() {
    const auto auto_size = parse_mask_size("auto");
    const auto cover = parse_mask_size("COVER");
    const auto one = parse_mask_size("30px");
    const auto two = parse_mask_size("50% 12px");
    const auto half_auto = parse_mask_size("20px auto");
    bool ok = expect(auto_size && auto_size->width.kind == MaskLength::Kind::Auto &&
                         auto_size->height.kind == MaskLength::Kind::Auto,
                     "auto is auto/auto");
    ok = expect(cover && cover->cover && !cover->contain, "cover is case-insensitive") && ok;
    ok = expect(one && one->width.kind == MaskLength::Kind::Px && near(one->width.value, 30.0f) &&
                    one->height.kind == MaskLength::Kind::Auto,
                "one length leaves the height auto") &&
         ok;
    ok = expect(two && two->width.kind == MaskLength::Kind::Percent &&
                    near(two->width.value, 0.5f) && two->height.kind == MaskLength::Kind::Px &&
                    near(two->height.value, 12.0f),
                "a percentage is a fraction, a second length is the height") &&
         ok;
    ok = expect(half_auto && half_auto->height.kind == MaskLength::Kind::Auto, "20px auto") && ok;
    for (const char* bad :
         {"", "-5px", "cover contain", "1px 2px 3px", "2em", "10", "auto auto auto"}) {
        ok = expect(!parse_mask_size(bad), "a size outside the grammar is rejected") && ok;
    }
    return ok;
}

bool positions() {
    const auto center = parse_mask_position("center");
    const auto corner = parse_mask_position("right bottom");
    const auto swapped = parse_mask_position("bottom right");
    const auto lengths = parse_mask_position("10px 25%");
    const auto mixed = parse_mask_position("left 7px");
    const auto edges = parse_mask_position("right 10px bottom 20%");
    const auto three = parse_mask_position("left 5px bottom");
    bool ok = expect(center && near(center->x.fraction, 0.5f) && near(center->y.fraction, 0.5f),
                     "one keyword centres the other axis");
    ok = expect(corner && near(corner->x.fraction, 1.0f) && near(corner->y.fraction, 1.0f),
                "right bottom") &&
         ok;
    ok = expect(swapped && near(swapped->x.fraction, 1.0f) && near(swapped->y.fraction, 1.0f),
                "a keyword pair may come in either order") &&
         ok;
    ok = expect(lengths && near(lengths->x.px, 10.0f) && near(lengths->y.fraction, 0.25f),
                "px is an offset, a percentage a fraction of the free space") &&
         ok;
    ok =
        expect(mixed && near(mixed->x.fraction, 0.0f) && near(mixed->y.px, 7.0f), "left 7px") && ok;
    ok = expect(edges && near(edges->x.fraction, 1.0f) && near(edges->x.px, -10.0f) &&
                    near(edges->y.fraction, 0.8f),
                "an offset from the right/bottom edge counts inwards") &&
         ok;
    ok = expect(three && near(three->x.px, 5.0f) && near(three->y.fraction, 1.0f),
                "three values name one edge offset and one bare edge") &&
         ok;
    for (const char* bad : {"", "left right", "top bottom", "10px left", "top 10px 20px", "1em",
                            "left top bottom", "a b c d e"}) {
        ok = expect(!parse_mask_position(bad),
                    (std::string("a position outside the grammar is rejected: ") + bad).c_str()) &&
             ok;
    }
    return ok;
}

bool repeats_and_modes() {
    const auto x = parse_mask_repeat("repeat-x");
    const auto pair = parse_mask_repeat("space round");
    const auto one = parse_mask_repeat("no-repeat");
    bool ok = expect(x && x->x == MaskRepeatKind::Repeat && x->y == MaskRepeatKind::NoRepeat,
                     "repeat-x is repeat no-repeat");
    ok = expect(pair && pair->x == MaskRepeatKind::Space && pair->y == MaskRepeatKind::Round,
                "two keywords are x then y") &&
         ok;
    ok = expect(one && one->x == MaskRepeatKind::NoRepeat && one->y == MaskRepeatKind::NoRepeat,
                "one keyword is both axes") &&
         ok;
    for (const char* bad : {"", "repeat-x repeat", "stretch", "repeat repeat repeat"}) {
        ok = expect(!parse_mask_repeat(bad), "a repeat outside the grammar is rejected") && ok;
    }
    ok = expect(parse_mask_mode("luminance") == MaskMode::Luminance &&
                    parse_mask_mode(" Alpha ") == MaskMode::Alpha &&
                    parse_mask_mode("match-source") == MaskMode::MatchSource &&
                    !parse_mask_mode("beta"),
                "mask-mode keywords") &&
         ok;
    return ok;
}

bool lists() {
    const auto list = parse_mask_size_list("10px, 20px 30px, cover");
    bool ok =
        expect(list && list->count == 3 && list->at_layer(1).width.kind == MaskLength::Kind::Px,
               "a list keeps one value per layer");
    // CSS repeats a short list over the image list.
    ok = expect(list && list->at_layer(3).width.kind == MaskLength::Kind::Px &&
                    near(list->at_layer(3).width.value, 10.0f) && list->at_layer(5).cover,
                "a short list repeats cyclically") &&
         ok;
    const MaskValueList<MaskMode> empty;
    ok =
        expect(empty.at_layer(7) == MaskMode::MatchSource, "an absent list is the initial value") &&
        ok;
    ok = expect(!parse_mask_size_list("10px,") && !parse_mask_size_list("") &&
                    !parse_mask_size_list("10px, nope") &&
                    !parse_mask_mode_list(
                        "alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha, alpha"),
                "an empty entry, a bad entry or more than 8 entries rejects the list") &&
         ok;
    return ok;
}

const std::string* find(const PropertyExpansionResult& result, const char* property) {
    for (const Declaration& declaration : result.declarations) {
        if (declaration.property == property) {
            return &declaration.value;
        }
    }
    return nullptr;
}

bool registry_longhands() {
    const PropertyRegistry registry;
    bool ok = true;
    for (const char* name : {"mask-size", "mask-position", "mask-repeat", "mask-mode",
                             "mask-origin", "mask-clip", "mask-composite"}) {
        ok = expect(registry.find(name) != nullptr, "the longhand is registered") && ok;
    }
    ok = expect(registry.find("mask-position")->initial == "0% 0%" &&
                    registry.find("mask-mode")->initial == "match-source",
                "initial values") &&
         ok;
    const auto good = registry.expand(Declaration{"mask-size", "20px, cover", false, 0});
    ok =
        expect(good.declarations.size() == 1 && good.diagnostics.empty(), "a valid list is kept") &&
        ok;
    for (const char* name : {"mask-size", "mask-position", "mask-repeat", "mask-mode",
                             "mask-origin", "mask-clip", "mask-composite"}) {
        const auto bad = registry.expand(Declaration{name, "wat wat wat wat wat", false, 0});
        ok = expect(bad.declarations.empty() && bad.diagnostics.size() == 1,
                    "an invalid list drops the declaration with one diagnostic") &&
             ok;
    }
    const auto substituted = registry.expand(Declaration{"mask-size", "var(--s)", false, 0});
    ok =
        expect(substituted.declarations.size() == 1, "a var() list is judged after substitution") &&
        ok;
    return ok;
}

bool boxes() {
    bool ok = expect(parse_mask_origin("padding-box") == MaskBox::Padding &&
                         parse_mask_origin(" CONTENT-BOX ") == MaskBox::Content &&
                         parse_mask_origin("border-box") == MaskBox::Border,
                     "the three boxes, case-insensitive");
    ok = expect(parse_mask_origin("fill-box") == MaskBox::Content &&
                    parse_mask_origin("stroke-box") == MaskBox::Border &&
                    parse_mask_origin("view-box") == MaskBox::Border,
                "the SVG boxes map as for an element with a CSS layout box") &&
         ok;
    ok = expect(parse_mask_clip("no-clip") == MaskBox::NoClip &&
                    parse_mask_clip("padding-box") == MaskBox::Padding,
                "mask-clip adds no-clip") &&
         ok;
    for (const char* bad : {"", "no-clip", "margin-box", "border-box padding-box", "10px"}) {
        ok = expect(!parse_mask_origin(bad), "mask-origin outside the grammar") && ok;
    }
    for (const char* bad : {"", "margin-box", "no-clip no-clip"}) {
        ok = expect(!parse_mask_clip(bad), "mask-clip outside the grammar") && ok;
    }
    const auto list = parse_mask_clip_list("padding-box, no-clip");
    ok = expect(list && list->count == 2 && list->at_layer(0) == MaskBox::Padding &&
                    list->at_layer(1) == MaskBox::NoClip && list->at_layer(2) == MaskBox::Padding,
                "a list, repeating cyclically") &&
         ok;
    ok = expect(!parse_mask_origin_list("padding-box, no-clip"), "no-clip is not an origin") && ok;
    return ok;
}

bool composites() {
    bool ok = expect(parse_mask_composite("add") == MaskComposite::Add &&
                         parse_mask_composite(" Subtract ") == MaskComposite::Subtract &&
                         parse_mask_composite("INTERSECT") == MaskComposite::Intersect &&
                         parse_mask_composite("exclude") == MaskComposite::Exclude,
                     "the four operators, case-insensitive");
    for (const char* bad : {"", "source-over", "add subtract", "xor", "10px"}) {
        ok = expect(!parse_mask_composite(bad), "an operator outside the grammar") && ok;
    }
    const auto list = parse_mask_composite_list("subtract, intersect");
    ok = expect(list && list->count == 2 && list->at_layer(0) == MaskComposite::Subtract &&
                    list->at_layer(1) == MaskComposite::Intersect &&
                    list->at_layer(2) == MaskComposite::Subtract,
                "a list, repeating cyclically") &&
         ok;
    const PropertyRegistry registry;
    const auto expand = [&](const char* value) {
        return registry.expand(Declaration{"mask", value, false, 0});
    };
    const auto composite = [](const auto& result) {
        const std::string* value = find(result, "mask-composite");
        return value == nullptr ? std::string("(none)") : *value;
    };
    ok = expect(
             composite(expand("linear-gradient(red, blue) subtract, linear-gradient(red, blue)")) ==
                 "subtract, add",
             "the shorthand names a layer's operator, and the other layer resets") &&
         ok;
    ok = expect(composite(expand("none")) == "add", "an unnamed operator resets") && ok;
    ok = expect(composite(expand("linear-gradient(red, blue) exclude padding-box luminance")) ==
                    "exclude",
                "in any component order") &&
         ok;
    return ok;
}

bool shorthand_boxes() {
    const PropertyRegistry registry;
    const auto expand = [&](const char* value) {
        return registry.expand(Declaration{"mask", value, false, 0});
    };
    const auto pair = [](const auto& result, const char* name) {
        const std::string* value = find(result, name);
        return value == nullptr ? std::string("(none)") : *value;
    };
    const auto one = expand("linear-gradient(red, blue) padding-box");
    bool ok =
        expect(pair(one, "mask-origin") == "padding-box" && pair(one, "mask-clip") == "padding-box",
               "one box sets origin and clip");
    const auto two = expand("linear-gradient(red, blue) content-box padding-box");
    ok =
        expect(pair(two, "mask-origin") == "content-box" && pair(two, "mask-clip") == "padding-box",
               "two boxes are origin then clip") &&
        ok;
    const auto no_clip = expand("linear-gradient(red, blue) content-box no-clip, none");
    ok =
        expect(pair(no_clip, "mask-origin") == "content-box, border-box" &&
                   pair(no_clip, "mask-clip") == "no-clip, border-box",
               "no-clip is the clip and the box before it is the origin; the other layer resets") &&
        ok;
    const auto alone = expand("linear-gradient(red, blue) no-clip");
    ok = expect(pair(alone, "mask-origin") == "border-box" && pair(alone, "mask-clip") == "no-clip",
                "no-clip alone leaves the origin initial") &&
         ok;
    const auto none = expand("none");
    ok =
        expect(pair(none, "mask-origin") == "border-box" && pair(none, "mask-clip") == "border-box",
               "an unnamed box resets both") &&
        ok;
    return ok;
}

bool shorthand() {
    const PropertyRegistry registry;
    const auto expand = [&](const char* value) {
        return registry.expand(Declaration{"mask", value, false, 0});
    };
    const auto full = expand(
        "linear-gradient(red, blue) right 10px bottom 5px / 30px 40px no-repeat luminance, none");
    bool ok = expect(full.diagnostics.empty() && full.declarations.size() == 8, "eight longhands");
    ok = expect(find(full, "mask-image") &&
                    *find(full, "mask-image") == "linear-gradient(red, blue), none",
                "the images keep their order") &&
         ok;
    ok = expect(find(full, "mask-position") &&
                    *find(full, "mask-position") == "right 10px bottom 5px, 0% 0%",
                "a layer's position, and the initial for the layer that names none") &&
         ok;
    ok =
        expect(find(full, "mask-size") && *find(full, "mask-size") == "30px 40px, auto", "sizes") &&
        ok;
    ok = expect(find(full, "mask-repeat") && *find(full, "mask-repeat") == "no-repeat, repeat",
                "repeats") &&
         ok;
    ok = expect(find(full, "mask-mode") && *find(full, "mask-mode") == "luminance, match-source",
                "modes") &&
         ok;

    const auto compact = expand("conic-gradient(#000, #fff) center/cover repeat-x alpha");
    ok = expect(compact.diagnostics.empty() && find(compact, "mask-position") &&
                    *find(compact, "mask-position") == "center" &&
                    *find(compact, "mask-size") == "cover" &&
                    *find(compact, "mask-repeat") == "repeat-x",
                "a slash without spaces, and any component order") &&
         ok;
    const auto in_color = expand("linear-gradient(rgb(0 0 0 / .5), #fff)");
    ok = expect(in_color.diagnostics.empty() && find(in_color, "mask-image") &&
                    *find(in_color, "mask-image") == "linear-gradient(rgb(0 0 0 / .5), #fff)",
                "a slash inside a colour is not the position/size slash") &&
         ok;
    const auto none = expand("none");
    ok = expect(none.diagnostics.empty() && find(none, "mask-image") &&
                    *find(none, "mask-image") == "none" && *find(none, "mask-size") == "auto",
                "none resets every longhand") &&
         ok;

    for (const char* bad :
         {"", "url(a.png)", "linear-gradient(red, blue) margin-box",
          "linear-gradient(red, blue) add subtract",
          "linear-gradient(red, blue) border-box padding-box content-box",
          "linear-gradient(red, blue) no-clip no-clip",
          "linear-gradient(red, blue) border-box padding-box no-clip",
          "linear-gradient(red, blue) / 10px", "linear-gradient(red, blue) 0 0 /",
          "linear-gradient(red, blue) alpha luminance",
          "linear-gradient(red, blue) repeat-x repeat", "linear-gradient(red, blue), , none",
          "linear-gradient(red, blue) 10px 10px 10px 10px 10px",
          "linear-gradient(red, blue) linear-gradient(red, blue)"}) {
        const auto rejected = expand(bad);
        ok = expect(rejected.declarations.empty() && !rejected.diagnostics.empty(),
                    "an unsupported mask rejects the whole declaration") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = sizes();
    ok = positions() && ok;
    ok = repeats_and_modes() && ok;
    ok = lists() && ok;
    ok = registry_longhands() && ok;
    ok = shorthand() && ok;
    ok = boxes() && ok;
    ok = shorthand_boxes() && ok;
    ok = composites() && ok;
    return ok ? 0 : 1;
}
