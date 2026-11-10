// SPDX-License-Identifier: MIT
//
// Headless integration demo: the compiler-generated `Demo` component is instantiated and built into
// a scene, laid out by Yoga, painted through the deterministic CPU canvas, driven by synthetic
// input, and repainted. This is the "generated UI is interactive" acceptance (task-56) plus the
// editable-text-field extension (task-59): the generated TextInput is focused, a scripted edit
// sequence runs through the real `tessera_text_edit` controller, and the controller's caret and
// half-open selection byte range are published as `PaintData::decoration` so the paint pass
// resolves them into line-box pixels.
//
// The generated code supplies the component struct (state cells/effects), the node tree, the
// Button's `PointerUp` binding (`handler_1` increments `state_count_`), the TextInput's `EditText`
// binding (`on_name` copies the committed text into `state_name_`) and the interpolated Text
// children that `apply_state()` refreshes; `update()` runs the changed effect. Controlled
// TextInput value/onChange wiring remains a documented codegen gap (docs/design/compiler.md §11):
// the demo publishes the edit buffer into the node itself instead.
//
// Usage: generated_ui_tests [--write] <font.ttf> <baseline_dir>

#include "generated_ui_demo.hpp"
#include "generated_ui_field_script.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/text/edit_controller.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace generated_ui_demo; // NOLINT: test-local helpers live in one namespace.
using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::ui::DirtyPass;
using tessera::ui::has_flag;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Modifier;
using tessera::ui::paint::FontPaintTextMetrics;
using tessera::ui::paint::PaintContext;
using tessera::ui::text::EditController;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "generated_ui: %s\n", message);
    }
    return condition;
}

using generated_ui_field_script::feed_key;
using generated_ui_field_script::run_field_script;

} // namespace

int main(int argc, char** argv) {
    bool write_mode = false;
    int argument = 1;
    if (argument < argc && std::string_view(argv[argument]) == "--write") {
        write_mode = true;
        ++argument;
    }
    if (argument + 1 >= argc) {
        std::fprintf(stderr, "usage: generated_ui_tests [--write] <font.ttf> <baseline_dir>\n");
        return 2;
    }
    const std::string font_path = argv[argument];
    const std::string baseline_dir = argv[argument + 1];

    auto loaded = tessera::ui::text::FontFace::load_from_file(font_path);
    if (!loaded) {
        std::fprintf(stderr, "generated_ui: failed to load font: %s\n",
                     loaded.error().message.c_str());
        return 2;
    }
    const tessera::ui::text::FontFace face = *loaded;
    GlyphRasterizer rasterizer(face);
    HarfBuzzShaper shaper(face);
    // Paint with the real-face metrics source so caret/selection advances match the glyphs the CPU
    // sink shapes, instead of the deterministic stub metrics the paint pass defaults to.
    FontPaintTextMetrics metrics(face, 1);
    PaintContext paint_context;
    paint_context.text_metrics_source = &metrics;

    // --- 1. Generated tree + Yoga layout + CPU paint -------------------------------------------
    Fixture fixture;
    if (!expect(fixture.layout_applied.applied == 6, "layout applied to every generated node") ||
        !expect(fixture.layout_applied.missing == 0 && fixture.layout_applied.extra == 0,
                "layout covered the tree exactly")) {
        return 3;
    }
    Node* const card = fixture.find(*fixture.root, "View");
    Node* const counter = fixture.find(*fixture.root, "Text");
    Node* const button = fixture.find(*fixture.root, "Button");
    Node* const text_input = fixture.find(*fixture.root, "TextInput");
    if (!expect(card != nullptr && counter != nullptr && button != nullptr && text_input != nullptr,
                "generated kinds are present") ||
        !expect(card->children().size() == 4,
                "the card has two Text children, a Button and an input")) {
        return 3;
    }
    Node* const name = card->children()[1].get();
    if (!expect(name->element_kind() == "Text", "the second Text is the Name interpolation") ||
        !expect(counter->paint_data().has_value() && counter->paint_data()->text == "Count: 0",
                "the generated build interpolated Count: 0")) {
        return 3;
    }
    if (!expect(counter->world_bounds().width > 0.0f, "text node laid out with a measured size") ||
        !expect(button->world_bounds().width == 96.0f, "button width from the style sheet")) {
        return 3;
    }
    // The sample sets no automationIds yet (documented codegen gap), so the demo resolves nodes by
    // element kind the way a generated automationId lookup would.
    if (!expect(button->automation_id() == 0, "sample sets no automationId (documented gap)")) {
        return 3;
    }
    // `{count > 0 && <Text>Positive</Text>}`: the branch is built once by build() and starts
    // detached, so the initial child set, the layout and the before frame are unchanged.
    if (!expect(fixture.demo.branch_6_owned_ != nullptr && fixture.demo.branch_6_node_ == nullptr,
                "the generated conditional branch is built once and starts detached")) {
        return 3;
    }

    CpuCanvas canvas(kWidth, kHeight);
    const std::vector<std::uint8_t> before =
        render(*fixture.root, fixture.styles, canvas, rasterizer, shaper, paint_context);
    const auto before_hash = fnv1a(before);
    if (!expect(fnv1a(render(*fixture.root, fixture.styles, canvas, rasterizer, shaper,
                             paint_context)) == before_hash,
                "two renders of the before frame are byte-identical")) {
        return 4;
    }
    if (write_mode) {
        if (!expect(save_png(baseline_dir + "/before.png", before), "write before golden")) {
            return 5;
        }
    } else if (!compare_png(baseline_dir + "/before.png", before, "before")) {
        return 5;
    }

    // --- 2. Click the generated Button: handler + route + state ---------------------------------
    InputRouter router;
    const auto click = [&](InputEventType type) {
        const auto bounds = button->world_bounds();
        return router.dispatch(*fixture.root, pointer_event(type, bounds.x + bounds.width * 0.5f,
                                                            bounds.y + bounds.height * 0.5f));
    };
    const auto down = click(InputEventType::PointerDown);
    const auto up = click(InputEventType::PointerUp);
    if (!expect(down.dispatched && up.dispatched, "pointer click dispatched") ||
        !expect(fixture.demo.state_count_ == 1, "generated handler incremented the state cell") ||
        !expect(route_is_w3c(*fixture.root, EventType::PointerDown), "PointerDown route is W3C") ||
        !expect(route_is_w3c(*fixture.root, EventType::PointerUp), "PointerUp route is W3C")) {
        return 6;
    }
    if (!expect(counter->paint_data().has_value() && counter->paint_data()->text == "Count: 1",
                "the generated setter interpolated Count: 1")) {
        return 6;
    }
    if (!expect(has_flag(counter->dirty(), Dirty::Paint) &&
                    has_flag(counter->dirty(), Dirty::Render),
                "the generated setter marks Paint and Render") ||
        !expect(!has_flag(counter->dirty(), Dirty::Layout),
                "text interpolation does not mark Layout (no reflow needed)")) {
        return 7;
    }
    fixture.demo.update();
    if (!expect(fixture.demo.effect_sync_state_.runs >= 1, "update() ran the generated effect")) {
        return 7;
    }

    DirtyPass dirty;
    dirty.collect(*fixture.root, Dirty::Paint | Dirty::Render);
    DamageAccumulator damage(DamageConfig{Extent2D{kWidth, kHeight}, 16});
    const auto damage_result = tessera::ui::layout::add_scene_damage(dirty, damage, 1.0f);
    const auto text_bounds = counter->world_bounds();
    bool damaged = false;
    for (const auto& rect : damage.rects()) {
        if (rect.x < text_bounds.x + text_bounds.width && rect.x + rect.width > text_bounds.x &&
            rect.y < text_bounds.y + text_bounds.height && rect.y + rect.height > text_bounds.y) {
            damaged = true;
        }
    }
    if (!expect(damage_result.nodes >= 1 && !damage.rects().empty(),
                "dirty paint produces damage") ||
        !expect(damaged, "the changed text node is inside the damage region")) {
        return 7;
    }

    // The conditional attaches at its source position (last child) once `count > 0`; the demo
    // re-runs layout so the attached branch gets a box and appears in the after frame.
    if (!expect(fixture.demo.branch_6_node_ != nullptr,
                "the generated conditional branch attached after the click") ||
        !expect(has_flag(card->dirty(), Dirty::Layout),
                "attaching the branch marks the parent Layout")) {
        return 7;
    }
    fixture.layout_scene();
    const auto branch_bounds = fixture.demo.branch_6_node_->world_bounds();
    if (!expect(card->children().size() == 5, "the attached branch is the fifth child") ||
        !expect(branch_bounds.width > 0.0f && branch_bounds.height > 0.0f,
                "the attached branch is laid out after the reflow") ||
        !expect(fixture.layout_applied.applied == 7, "layout covered the attached branch")) {
        return 7;
    }

    const std::vector<std::uint8_t> after =
        render(*fixture.root, fixture.styles, canvas, rasterizer, shaper, paint_context);
    const auto after_hash = fnv1a(after);
    if (!expect(after != before, "the repaint differs after the state change") ||
        !expect(fnv1a(render(*fixture.root, fixture.styles, canvas, rasterizer, shaper,
                             paint_context)) == after_hash,
                "two renders of the after frame are byte-identical")) {
        return 8;
    }
    if (write_mode) {
        if (!expect(save_png(baseline_dir + "/after.png", after), "write after golden")) {
            return 9;
        }
    } else if (!compare_png(baseline_dir + "/after.png", after, "after")) {
        return 9;
    }

    // --- 3. TextInput: generated EditText handler + the edit controller -------------------------
    if (!expect(router.focus_manager().request_focus(text_input) &&
                    router.focus_manager().focused() == text_input,
                "TextInput focused")) {
        return 10;
    }
    const InputEvent typed = edit_text_event("Ada");
    const auto typed_result = router.dispatch(*fixture.root, typed);
    EditController& generated_editor = fixture.demo.text_input_editor(0);
    if (!expect(typed_result.dispatched, "EditText routed to the focused TextInput") ||
        !expect(route_is_w3c(*fixture.root, EventType::EditText),
                "EditText route is W3C capture/target/bubble") ||
        !expect(route_is_w3c(*fixture.root, EventType::TextChange),
                "the runtime TextChange dispatch is W3C capture/target/bubble") ||
        !expect(fixture.demo.state_name_ == "Ada",
                "generated onChange copied the committed text into state") ||
        !expect(generated_editor.buffer().text() == "Ada",
                "the generated TextInputRuntime owns the edit buffer") ||
        !expect(text_input->paint_data().has_value() && text_input->paint_data()->text == "Ada",
                "apply_state painted the controlled value through generated code only") ||
        !expect(name->paint_data().has_value() && name->paint_data()->text == "Name: Ada",
                "the state change interpolated Name: Ada") ||
        !expect(has_flag(name->dirty(), Dirty::Paint),
                "typing marks the interpolated Name text Paint") ||
        !expect(fixture.demo.text_input_change_events(0) == 1,
                "exactly one TextChange event per committed edit")) {
        return 10;
    }

    // Preedit (CompositionUpdate) must not emit a change event, and a non-mutating auto-repeat
    // key must not either; the generated text stays as committed.
    InputEvent preedit;
    preedit.type = InputEventType::CompositionUpdate;
    preedit.text[0] = 'x';
    preedit.text_size = 1;
    router.dispatch(*fixture.root, preedit);
    InputEvent repeat;
    repeat.type = InputEventType::KeyDown;
    repeat.key = tessera::ui::key::kArrowLeft;
    repeat.repeat = true;
    router.dispatch(*fixture.root, repeat);
    if (!expect(fixture.demo.text_input_change_events(0) == 1,
                "preedit and non-mutating auto-repeat emit no TextChange") ||
        !expect(text_input->paint_data().value_or(tessera::ui::PaintData{}).text == "Ada",
                "preedit/auto-repeat do not change the painted text")) {
        return 11;
    }
    const std::vector<std::uint8_t> typed_render =
        render(*fixture.root, fixture.styles, canvas, rasterizer, shaper, paint_context);
    if (!expect(typed_render != after, "typing repaints through generated interpolation")) {
        return 11;
    }

    // --- 4. Editable field: edit model -> PaintData decoration -> pixels ------------------------
    EditController& field = generated_editor;
    field.set_text("");
    if (!expect(run_field_script(router, *fixture.root, field),
                "field edit script reached the editable frame") ||
        !expect(field.buffer().text() == "Ada Lovelace", "field text") ||
        !expect(field.buffer().caret() == 12, "caret after the selection") ||
        !expect(field.buffer().selection_start() == 4, "selection starts after Ada ") ||
        !expect(text_input->paint_data().has_value() &&
                    text_input->paint_data()->text == "Ada Lovelace",
                "every committed edit repainted the generated controlled text")) {
        return 12;
    }
    publish_decoration(*text_input, field.buffer());
    if (!expect(has_flag(text_input->dirty(), Dirty::Paint) &&
                    has_flag(text_input->dirty(), Dirty::Render),
                "publishing the edit state marks Paint and Render") ||
        !expect(!has_flag(text_input->dirty(), Dirty::Layout),
                "publishing text/decoration does not mark Layout")) {
        return 12;
    }

    const std::vector<std::uint8_t> editing =
        render(*fixture.root, fixture.styles, canvas, rasterizer, shaper, paint_context);
    const auto editing_hash = fnv1a(editing);
    if (!expect(editing != after, "the editing frame differs from the after frame")) {
        return 13;
    }

    const DecorationRects rects = decoration_rects(*fixture.root, fixture.styles, paint_context);
    const auto content = text_content_box(*text_input);
    if (!expect(rects.carets.size() == 1, "one caret rect emitted") ||
        !expect(rects.selection.size() == 1, "one selection rect emitted") ||
        !expect(rect_inside(rects.carets[0], content),
                "the caret rect is inside the input content box") ||
        !expect(rect_inside(rects.selection[0], content),
                "the selection rect is inside the input content box") ||
        !expect(rects.selection[0].width > 0.0f, "the selection highlight has width")) {
        return 13;
    }
    if (!expect(rects.carets[0].x == rects.selection[0].x + rects.selection[0].width,
                "the caret sits at the right edge of the selection (half-open range)")) {
        return 13;
    }

    // Typing moves the caret: the caret at byte 0 is left of the caret after the whole text.
    const std::string field_text(field.buffer().text());
    publish_caret_decoration(*text_input, 0);
    const DecorationRects at_start = decoration_rects(*fixture.root, fixture.styles, paint_context);
    publish_caret_decoration(*text_input, field_text.size());
    const DecorationRects at_end = decoration_rects(*fixture.root, fixture.styles, paint_context);
    if (!expect(at_start.carets.size() == 1 && at_end.carets.size() == 1,
                "caret-only decoration emits one caret each") ||
        !expect(at_start.selection.empty() && at_end.selection.empty(),
                "no selection fill when the selection is collapsed") ||
        !expect(at_start.carets[0].x < at_end.carets[0].x,
                "a later caret is to the right of an earlier one") ||
        !expect(rect_inside(at_start.carets[0], content), "start caret inside the content box") ||
        !expect(rect_inside(at_end.carets[0], content), "end caret inside the content box")) {
        return 14;
    }

    // Delete-selection collapses the caret and removes the highlight: the script leaves
    // "Lovelace" selected, so Backspace removes it and leaves "Ada " with the caret after it.
    if (!expect(field.buffer().has_selection(), "the field has a selection to delete") ||
        !feed_key(router, *fixture.root, tessera::ui::key::kBackspace) ||
        !expect(field.buffer().text() == "Ada ", "delete removed the selected text") ||
        !expect(!field.buffer().has_selection() && field.buffer().caret() == 4,
                "delete collapsed the caret to the end of the remaining text") ||
        !expect(text_input->paint_data().has_value() && text_input->paint_data()->text == "Ada ",
                "delete repainted the generated controlled text")) {
        return 15;
    }
    publish_decoration(*text_input, field.buffer());
    const DecorationRects collapsed_rects =
        decoration_rects(*fixture.root, fixture.styles, paint_context);
    if (!expect(collapsed_rects.selection.empty(),
                "deleting the selection removes the highlight") ||
        !expect(collapsed_rects.carets.size() == 1 &&
                    rect_inside(collapsed_rects.carets[0], content),
                "the collapsed caret is inside the content box")) {
        return 15;
    }
    // Restore the editable frame through the generated path, then decorate only.
    field.set_text("");
    if (!expect(run_field_script(router, *fixture.root, field),
                "restored edit script reached the editable frame")) {
        return 15;
    }
    publish_decoration(*text_input, field.buffer());

    if (write_mode) {
        if (!expect(save_png(baseline_dir + "/editing.png", editing), "write editing golden")) {
            return 16;
        }
    } else if (!compare_png(baseline_dir + "/editing.png", editing, "editing")) {
        return 16;
    }
    if (!expect(fnv1a(render(*fixture.root, fixture.styles, canvas, rasterizer, shaper,
                             paint_context)) == editing_hash,
                "two renders of the editing frame are byte-identical")) {
        return 17;
    }

    // --- 5. Determinism: rebuild the whole generated UI and reproduce the frames ----------------
    Fixture replay;
    Node* replay_button = replay.find(*replay.root, "Button");
    Node* replay_input = replay.find(*replay.root, "TextInput");
    InputRouter replay_router;
    const auto replay_click = [&](InputEventType type) {
        const auto bounds = replay_button->world_bounds();
        replay_router.dispatch(*replay.root, pointer_event(type, bounds.x + bounds.width * 0.5f,
                                                           bounds.y + bounds.height * 0.5f));
    };
    replay_click(InputEventType::PointerDown);
    replay_click(InputEventType::PointerUp);
    replay.demo.update();
    // Same reflow as the first run: the attached conditional branch needs a box in the replay too.
    replay.layout_scene();
    replay_router.focus_manager().request_focus(replay_input);
    // Replay the generated onChange commit too, so the interpolated Name text matches.
    replay_router.dispatch(*replay.root, typed);
    EditController& replay_field = replay.demo.text_input_editor(0);
    replay_field.set_text("");
    if (!expect(run_field_script(replay_router, *replay.root, replay_field),
                "replayed edit script") ||
        !expect(replay_field.buffer().text() == field_text, "replayed buffer text")) {
        return 18;
    }
    publish_decoration(*replay_input, replay_field.buffer());
    CpuCanvas replay_canvas(kWidth, kHeight);
    const std::vector<std::uint8_t> replay_editing =
        render(*replay.root, replay.styles, replay_canvas, rasterizer, shaper, paint_context);
    if (!expect(replay_editing == editing, "a rebuilt generated UI reproduces the editing frame") ||
        !expect(replay.demo.state_count_ == 1, "replayed click state")) {
        return 18;
    }

    std::printf("generated_ui: ok before=%016llx after=%016llx editing=%016llx\n",
                static_cast<unsigned long long>(before_hash),
                static_cast<unsigned long long>(after_hash),
                static_cast<unsigned long long>(editing_hash));
    return 0;
}
