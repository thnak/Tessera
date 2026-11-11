// SPDX-License-Identifier: MIT
//
// Font stacks from an app bundle (issue #12): the stack manifest lists font assets in fallback
// order, every failure is a value naming the asset, and the loaded stack drives FallbackShaper so
// text one face lacks falls through to the next (the emoji face last).

#include "asset_font_fixtures_assets.hpp"
#include "tessera/ui/assets/asset_font.hpp"
#include "tessera/ui/text/fallback_shaper.hpp"

#include <iostream>
#include <utility>

namespace {

namespace assets = tessera::ui::assets;
namespace text = tessera::ui::text;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "asset_font: " << message << "\n";
    }
    return condition;
}

bool stack_loads_in_order() {
    const auto bundle = assets::font_fixtures::bundle();
    auto stack = assets::load_font_stack(bundle);
    bool ok = expect(stack.has_value(), "the default stack manifest loads");
    if (!ok) {
        return false;
    }
    // CRLF-free comments, blank lines and padding are ignored; order is the manifest's.
    ok = expect(stack->size() == 2, "two faces are listed") &&
         expect((*stack)[0].has_glyph(U'א') && !(*stack)[0].has_glyph(U'\U0001F600'),
                "the primary face is Hebrew") &&
         expect((*stack)[1].has_glyph(U'\U0001F600') && !(*stack)[1].has_glyph(U'א'),
                "the emoji face is last");

    const text::FallbackShaper shaper(std::move(*stack));
    const auto covered = shaper.shape("\xD7\x90\xF0\x9F\x98\x80", text::TextStyle{});
    ok = expect(covered.glyphs.size() == 2 && covered.missing_glyphs == 0,
                "Hebrew and emoji both resolve through the stack") &&
         ok;
    const auto uncovered = shaper.shape("\xE0\xB8\x81", text::TextStyle{}); // Thai KO KAI
    ok = expect(uncovered.missing_glyphs == 1, "a character no face covers is counted") && ok;
    return ok;
}

bool single_fonts() {
    const auto bundle = assets::font_fixtures::bundle();
    bool ok = expect(assets::load_asset_font(bundle, "fonts/ColorTest.ttf").has_value(),
                     "a font asset loads by name");
    const auto missing = assets::load_asset_font(bundle, "fonts/nope.ttf");
    ok = expect(!missing && missing.error().error == assets::AssetFontError::not_found &&
                    missing.error().asset == "fonts/nope.ttf",
                "a missing font names the asset") &&
         ok;
    const auto invalid = assets::load_asset_font(bundle, "fonts/../x");
    ok = expect(!invalid && invalid.error().error == assets::AssetFontError::invalid_name,
                "an invalid name is reported") &&
         ok;
    const auto not_font = assets::load_asset_font(bundle, "fonts/not-a-font.txt");
    ok = expect(!not_font && not_font.error().error == assets::AssetFontError::invalid_font &&
                    not_font.error().asset == "fonts/not-a-font.txt",
                "bytes that are not a font are rejected") &&
         ok;
    return ok;
}

bool broken_stacks() {
    const auto bundle = assets::font_fixtures::bundle();
    const auto no_manifest = assets::load_font_stack(bundle, "fonts/absent.txt");
    bool ok = expect(!no_manifest && no_manifest.error().error == assets::AssetFontError::not_found,
                     "a missing manifest is not_found");
    const auto bad = assets::load_font_stack(bundle, "fonts/bad-stack.txt");
    ok = expect(!bad && bad.error().error == assets::AssetFontError::invalid_font &&
                    bad.error().asset == "fonts/not-a-font.txt",
                "one bad face fails the stack and names it") &&
         ok;
    const auto missing = assets::load_font_stack(bundle, "fonts/missing-stack.txt");
    ok = expect(!missing && missing.error().error == assets::AssetFontError::not_found &&
                    missing.error().asset == "fonts/nope.ttf",
                "a listed face that is not in the bundle fails the stack") &&
         ok;
    const auto empty = assets::load_font_stack(bundle, "fonts/empty-stack.txt");
    ok = expect(!empty && empty.error().error == assets::AssetFontError::empty_stack,
                "a manifest with no face is an empty stack") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = stack_loads_in_order();
    ok = single_fonts() && ok;
    ok = broken_stacks() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "asset_font: ok\n";
    return 0;
}
