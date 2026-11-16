// SPDX-License-Identifier: MIT
//
// Preview canvas text (issue #387): the font loaded as data, painted through the real text stack.
//
// Split from `scene_frame_tests.cpp` for the source-size limit, not for a different subject: that
// file proves the frame path (prepare, counters, determinism, the report, the tolerance policy) and
// this one proves the *text* half of it. Both are built into the one `scene_frame` ctest case by
// `src/compiler/codegen/CMakeLists.txt`, and they share `scene_frame_test_support.hpp` so the
// failure counter is defined once.
//
// The acceptance is "the preview paints real text instead of a box counted in `textSkipped`, with
// the font shipped as data". What these cases prove on a host without WebGPU:
//
//  * a frame painted *with* font bytes records glyph geometry and reports `text_skipped == 0`,
//  while
//    the same scene without a font still counts its runs - so the two states are distinguishable
//    and neither is silently "empty";
//  * two frames from the same bytes record the identical command stream, which is what makes a
//    golden possible at all;
//  * the face is chosen by the bytes, not by the host: an unusable buffer is a named failure that
//    leaves the preview in its degraded (box-only) state;
//  * a bounded atlas reports what it could not place, instead of silently growing or silently
//    dropping ink.

#include "scene_frame_test_support.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scene_frame_test::expect;
using scene_frame_test::kColumn;
using scene_frame_test::options_for;
using scene_frame_test::shipped_font;

// The aliases the shared header also declares; redeclared because a namespace alias is not
// reachable through a `using`-declaration (the same reason `scene_frame_tests.cpp` repeats them).
namespace cli = tessera::compiler::codegen::cli;
namespace null = tessera::rendering::null;
namespace rhi = tessera::rendering::rhi;

// A source with more distinct glyphs than a 32x32 one-page atlas can hold, so the drop path is
// reached by text that is laid out normally rather than by a contrived API call.
constexpr std::string_view kGlyphSoup = R"(export function GlyphSoup(): Node {
  return (
    <View>
      <Text>ABCDEFGHIJKLMNOPQRSTUVWXYZ</Text>
      <Text>abcdefghijklmnopqrstuvwxyz</Text>
      <Text>0123456789 !?&amp;@#$%</Text>
    </View>
  );
}
)";

void test_text_paints_with_a_font() {
    const std::vector<std::uint8_t> bytes = shipped_font();
    if (bytes.empty()) {
        expect(false, "the shipped font is readable at designer/web/public/fonts/");
        return;
    }
    std::unique_ptr<rhi::Device> device = null::create_null_device(rhi::DeviceDesc{});
    if (device == nullptr) {
        expect(false, "the null device is created");
        return;
    }
    auto text = cli::PreviewTextStack::create(bytes, *device);
    expect(text.has_value(), "the shipped font loads as bytes");
    if (!text) {
        return;
    }

    // The face is the bytes', not the host's.
    expect((*text)->face_name() == "IBM Plex Sans",
           "the loaded face reports the family the bytes carry, got `" + (*text)->face_name() +
               "`");

    const auto with_font = cli::scene_frame_render(kColumn, options_for("ui/Column.tsx"), *device,
                                                   {320, 240}, {}, {}, text->get());
    const auto without =
        cli::scene_frame_render(kColumn, options_for("ui/Column.tsx"), *device, {320, 240});
    expect(with_font.error.empty(), "a frame with a font renders: " + with_font.error);
    expect(without.error.empty(), "a frame without a font still renders: " + without.error);

    // The acceptance, as one assertion each way.
    expect(without.text_skipped >= 1, "the font-free frame still counts its runs as skipped");
    expect(with_font.text_skipped == 0, "a frame with a font skips no text");
    expect(with_font.texts >= 1, "a frame with a font emits text runs");
    expect(with_font.glyphs_dropped == 0, "a default-sized atlas drops nothing for this sample");

    // Glyph quads are real geometry: every glyph is a textured quad, so a frame that paints three
    // boxes and then its text records strictly more vertices than the font-free one.
    expect(with_font.vertex_count > without.vertex_count,
           "text adds glyph vertices (" + std::to_string(with_font.vertex_count) + " vs " +
               std::to_string(without.vertex_count) + ")");
    expect(with_font.draw_calls > without.draw_calls,
           "text adds draw calls (" + std::to_string(with_font.draw_calls) + " vs " +
               std::to_string(without.draw_calls) + ")");

    // A button's caption lives in its `label` prop, not its `text` (the lowering rule), so a
    // preview that painted only `Text` payloads would draw an empty button. `kColumn` has two
    // buttons and one `Text`; a frame that reports fewer than three runs left a caption unpainted.
    expect(with_font.texts >= 3, "all three runs paint, including both button captions (got " +
                                     std::to_string(with_font.texts) + ")");
}

void test_text_is_deterministic() {
    const std::vector<std::uint8_t> bytes = shipped_font();
    if (bytes.empty()) {
        expect(false, "the shipped font is readable for the determinism case");
        return;
    }
    std::unique_ptr<rhi::Device> first_device = null::create_null_device(rhi::DeviceDesc{});
    std::unique_ptr<rhi::Device> second_device = null::create_null_device(rhi::DeviceDesc{});
    if (first_device == nullptr || second_device == nullptr) {
        expect(false, "two null devices are created");
        return;
    }
    auto first_text = cli::PreviewTextStack::create(bytes, *first_device);
    auto second_text = cli::PreviewTextStack::create(bytes, *second_device);
    expect(first_text.has_value() && second_text.has_value(), "both stacks load the same bytes");
    if (!first_text || !second_text) {
        return;
    }

    const auto a = cli::scene_frame_render(kColumn, options_for("ui/Column.tsx"), *first_device,
                                           {320, 240}, {}, {}, first_text->get());
    const auto b = cli::scene_frame_render(kColumn, options_for("ui/Column.tsx"), *second_device,
                                           {320, 240}, {}, {}, second_text->get());
    expect(a.error.empty() && b.error.empty(), "both font frames render");
    // The whole point of loading the font as data: the command stream is a function of (source,
    // bytes), so two independent devices record the same one.
    expect(a.draw_calls == b.draw_calls, "two font frames record the same draw-call count");
    expect(a.vertex_count == b.vertex_count, "two font frames record the same vertex count");
    expect(a.texts == b.texts && a.text_skipped == b.text_skipped,
           "two font frames report the same text counters");

    // A different face is a different frame. Only the *bytes* decide what is painted, which is what
    // rules out a host font standing in for them.
    const auto none =
        cli::scene_frame_render(kColumn, options_for("ui/Column.tsx"), *first_device, {320, 240});
    expect(a.vertex_count != none.vertex_count, "the font changes the frame");
}

void test_font_failures_are_named() {
    std::unique_ptr<rhi::Device> device = null::create_null_device(rhi::DeviceDesc{});
    if (device == nullptr) {
        expect(false, "the null device is created for the failure cases");
        return;
    }
    // Empty data is its own code, so a page can tell "the fetch returned nothing" from "the bytes
    // are not a font" - the two need different sentences.
    const auto empty = cli::PreviewTextStack::create({}, *device);
    expect(!empty.has_value(), "empty font data is refused");
    if (!empty) {
        expect(empty.error().code == cli::PreviewFontErrorCode::empty_data,
               "empty data reports `empty_data`");
    }
    // Not-a-font is refused by FreeType, through the production loader.
    const std::vector<std::uint8_t> garbage(4096, std::uint8_t{0x7F});
    const auto junk = cli::PreviewTextStack::create(garbage, *device);
    expect(!junk.has_value(), "bytes that are not a font are refused");
    if (!junk) {
        expect(junk.error().code == cli::PreviewFontErrorCode::invalid_font,
               "a non-font reports `invalid_font`");
        expect(!junk.error().message.empty(), "the refusal carries a message for the page");
    }
    // A refused font leaves the preview usable: the same source still paints its boxes.
    const auto degraded =
        cli::scene_frame_render(kColumn, options_for("ui/Column.tsx"), *device, {320, 240});
    expect(degraded.error.empty(), "a scene still renders after a font failure");
    expect(degraded.backgrounds >= 1, "the degraded frame still paints its boxes");
    expect(degraded.text_skipped >= 1, "the degraded frame reports its runs as skipped");
}

void test_atlas_bounds_are_reported() {
    const std::vector<std::uint8_t> bytes = shipped_font();
    if (bytes.empty()) {
        expect(false, "the shipped font is readable for the atlas case");
        return;
    }
    std::unique_ptr<rhi::Device> device = null::create_null_device(rhi::DeviceDesc{});
    if (device == nullptr) {
        expect(false, "the null device is created for the atlas case");
        return;
    }
    // One 32x32 page: a couple of 16-pixel glyphs and no more. This is the bounded resource the
    // header documents, made small enough that ordinary text exhausts it.
    tessera::ui::text::GlyphAtlasConfig tiny;
    tiny.page_width = 32;
    tiny.page_height = 32;
    tiny.max_pages = 1;
    auto text = cli::PreviewTextStack::create(bytes, *device, tiny);
    expect(text.has_value(), "the stack loads with a deliberately tiny atlas");
    if (!text) {
        return;
    }
    const auto squeezed = cli::scene_frame_render(kGlyphSoup, options_for("ui/GlyphSoup.tsx"),
                                                  *device, {320, 240}, {}, {}, text->get());
    expect(squeezed.error.empty(), "an exhausted atlas does not fail the frame: " + squeezed.error);
    // The consequence, precisely: the text is still *laid out and drawn* (runs are not skipped),
    // and the glyphs that did not fit are counted. The frame is therefore neither blank nor
    // silently wrong, which is the whole contract of a bounded resource.
    expect(squeezed.text_skipped == 0,
           "an exhausted atlas still draws text, so nothing is skipped");
    expect(squeezed.texts >= 1, "the runs are emitted even when glyphs drop");
    expect(squeezed.glyphs_dropped > 0, "glyphs the atlas could not place are counted (got " +
                                            std::to_string(squeezed.glyphs_dropped) + ")");
    // The atlas never grew past its configured bound.
    expect((*text)->counters().atlas_pages <= 1, "the atlas stays within its page bound");

    // The same source at the default size drops nothing, so the counter means "the atlas was too
    // small", not "this text is unrenderable".
    auto roomy = cli::PreviewTextStack::create(bytes, *device);
    if (roomy) {
        const auto normal = cli::scene_frame_render(kGlyphSoup, options_for("ui/GlyphSoup.tsx"),
                                                    *device, {320, 240}, {}, {}, roomy->get());
        expect(normal.glyphs_dropped == 0,
               "the default atlas holds the same text, so the drop was a size limit");
        expect(normal.vertex_count > squeezed.vertex_count,
               "the frame with every glyph draws more geometry than the squeezed one");
    }
}

} // namespace

namespace scene_frame_test {

void run_text_cases() {
    test_text_paints_with_a_font();
    test_text_is_deterministic();
    test_font_failures_are_named();
    test_atlas_bounds_are_reported();
}

} // namespace scene_frame_test
