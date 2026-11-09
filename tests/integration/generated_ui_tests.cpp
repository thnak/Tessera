// SPDX-License-Identifier: MIT
//
// Headless integration demo: the compiler-generated `Demo` component is instantiated and built into
// a scene, laid out by Yoga, painted through the deterministic CPU canvas, driven by synthetic
// input, and repainted. This is the "generated UI is interactive" acceptance (task-56).
//
// The generated code supplies the component struct (state cells/effects), the node tree, the
// Button's `PointerUp` binding (`handler_1` increments `state_count_`), the TextInput's `EditText`
// binding (`on_name` copies the committed text into `state_name_`) and the interpolated Text
// children (`Count: {count}`, `Name: {name}`) that the generated setters/regions refresh through
// `apply_state()`; `update()` runs the changed effect. Controlled TextInput value/onChange wiring
// remains a documented codegen gap.
//
// Usage: generated_ui_tests [--write] <font.ttf> <baseline_dir>

#include "generated_ui_demo.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
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
using tessera::ui::InputRouter;
using tessera::ui::text::EditController;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "generated_ui: %s\n", message);
    }
    return condition;
}

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
    if (!expect(name->element_kind() == "Text", "the second Text is the Name interpolation")) {
        return 3;
    }
    if (!expect(counter->paint_data().has_value() && counter->paint_data()->text == "Count: 0",
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

    CpuCanvas canvas(kWidth, kHeight);
    const std::vector<std::uint8_t> before =
        render(*fixture.root, fixture.styles, canvas, rasterizer, shaper);
    const auto before_hash = fnv1a(before);
    if (!expect(fnv1a(render(*fixture.root, fixture.styles, canvas, rasterizer, shaper)) ==
                    before_hash,
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

    const std::vector<std::uint8_t> after =
        render(*fixture.root, fixture.styles, canvas, rasterizer, shaper);
    const auto after_hash = fnv1a(after);
    if (!expect(after != before, "the repaint differs after the state change") ||
        !expect(fnv1a(render(*fixture.root, fixture.styles, canvas, rasterizer, shaper)) ==
                    after_hash,
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
    if (!expect(typed_result.dispatched, "EditText routed to the focused TextInput") ||
        !expect(route_is_w3c(*fixture.root, EventType::EditText),
                "EditText route is W3C capture/target/bubble") ||
        !expect(fixture.demo.state_name_ == "Ada",
                "generated on_name handler copied the committed text into state") ||
        !expect(name->paint_data().has_value() && name->paint_data()->text == "Name: Ada",
                "the developer region's state change interpolated Name: Ada") ||
        !expect(has_flag(name->dirty(), Dirty::Paint),
                "typing marks the interpolated Name text Paint")) {
        return 10;
    }

    // The same committed text through the edit controller's buffer (the owner a future controlled
    // TextInput would bind to; there is no generated value/onChange wiring yet - documented gap).
    EditController editor;
    Event commit;
    commit.type = EventType::EditText;
    std::copy_n(typed.text.begin(), typed.text_size, commit.text.begin());
    commit.text_size = typed.text_size;
    if (!expect(editor.handle_event(commit), "edit controller consumed the commit") ||
        !expect(editor.buffer().text() == "Ada", "edit buffer holds the typed value") ||
        !expect(editor.buffer().text() == fixture.demo.state_name_,
                "buffer and generated state agree for the same input")) {
        return 11;
    }
    const std::vector<std::uint8_t> typed_render =
        render(*fixture.root, fixture.styles, canvas, rasterizer, shaper);
    if (!expect(typed_render != after, "typing repaints through generated interpolation")) {
        return 11;
    }

    // --- 4. Determinism: rebuild the whole generated UI and reproduce the frames ----------------
    Fixture replay;
    Node* replay_button = replay.find(*replay.root, "Button");
    InputRouter replay_router;
    const auto replay_click = [&](InputEventType type) {
        const auto bounds = replay_button->world_bounds();
        replay_router.dispatch(*replay.root, pointer_event(type, bounds.x + bounds.width * 0.5f,
                                                           bounds.y + bounds.height * 0.5f));
    };
    replay_click(InputEventType::PointerDown);
    replay_click(InputEventType::PointerUp);
    CpuCanvas replay_canvas(kWidth, kHeight);
    const std::vector<std::uint8_t> replay_after =
        render(*replay.root, replay.styles, replay_canvas, rasterizer, shaper);
    if (!expect(replay_after == after, "a rebuilt generated UI reproduces the after frame") ||
        !expect(replay.demo.state_count_ == 1, "replayed click state")) {
        return 12;
    }

    std::printf("generated_ui: ok before=%016llx after=%016llx\n",
                static_cast<unsigned long long>(before_hash),
                static_cast<unsigned long long>(after_hash));
    return 0;
}
