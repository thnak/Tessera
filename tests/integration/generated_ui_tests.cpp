// SPDX-License-Identifier: MIT
//
// Headless integration demo: the compiler-generated `Demo` component is instantiated and built into
// a scene, laid out by Yoga, painted through the deterministic CPU canvas, driven by synthetic
// input, and repainted. This is the "generated UI is interactive" acceptance (task-56).
//
// What is generated: the component struct (state cells/effects), the node tree, the Button's
// `PointerUp` binding (handler::1 increments `state_count_`) and the TextInput's `EditText` binding
// (`on_name` developer region copies the event text into `state_name_`).
//
// What codegen does not emit yet (documented gaps, asserted below where applicable): state
// interpolation into node content (so a click alone cannot change pixels until a regeneration step
// writes the counter into the Text node's PaintData) and controlled `value` -> PaintData wiring for
// TextInput. The demo applies the interpolation itself to prove the input -> state -> repaint path.
//
// Usage: generated_ui_tests [--write] <font.ttf> <baseline_dir>

#include "Demo.hpp"
#include "cpu_paint_sink.hpp"
#include "stb_image.h"
#include "stb_image_write.h"
#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/edit_controller.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::rendering::golden::compare_rgba;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::has_flag;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::PointerButton;
using tessera::ui::PointerKind;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::EditController;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::TextShaper;

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 200;
constexpr tessera::ui::NodeId kRootId = 900;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "generated_ui: " << message << "\n";
    }
    return condition;
}

// Deterministic demo styles: the generated nodes carry element kinds/class names only (inline
// style attachment is a documented codegen gap), so the test supplies the style sheet values the
// component author wrote in JSX.
class DemoSettings final : public StyleProvider {
public:
    [[nodiscard]] ComputedStyle compute(const Node& node) override {
        ComputedStyle style;
        const std::string_view kind = node.element_kind();
        if (kind.empty()) {
            style.set("width", "320px");
            style.set("height", "200px");
            style.set("display", "flex");
            style.set("flex-direction", "column");
            style.set("padding", "16px");
            style.set("background-color", "#0b1220");
        } else if (kind == "View") {
            style.set("width", "240px");
            style.set("height", "150px");
            style.set("display", "flex");
            style.set("flex-direction", "column");
            style.set("padding", "12px");
            style.set("background-color", "#1e293b");
        } else if (kind == "Text") {
            style.set("font-size", "16px");
            style.set("color", "#e2e8f0");
            style.set("margin-bottom", "6px");
        } else if (kind == "Button") {
            style.set("width", "96px");
            style.set("height", "28px");
            style.set("margin-bottom", "8px");
            style.set("background-color", "#2563eb");
        } else if (kind == "TextInput") {
            style.set("width", "180px");
            style.set("height", "28px");
            style.set("background-color", "#0f172a");
        }
        return style;
    }
};

// Records the event route (node id + phase) so the W3C capture/target/bubble order is asserted.
class RouteRecorder final : public Node {
public:
    using Node::Node;

    void on_event(Event& event) override {
        route.push_back({event.type, event.phase});
    }

    struct Entry {
        EventType type = EventType::PointerDown;
        EventPhase phase = EventPhase::Target;
    };
    std::vector<Entry> route;
};

struct Fixture {
    Fixture() {
        root = std::make_unique<RouteRecorder>(kRootId);
        demo.build(*root);
        demo.mount();
        layout_scene();
    }

    void layout_scene() {
        tessera::ui::layout::LayoutEngine engine;
        tessera::ui::layout::LayoutInput input = build_layout_input(*root);
        const tessera::ui::layout::LayoutResult result = engine.layout(
            input, tessera::ui::layout::LayoutViewport{static_cast<float>(kWidth),
                                                       static_cast<float>(kHeight), 1.0f});
        const tessera::ui::layout::LayoutApplyResult applied =
            tessera::ui::layout::apply_layout(result, *root);
        layout_applied = applied;
        // apply_layout writes bounds (which marks Paint|Render); a frame would clear those after
        // painting, so start the interaction assertions from a clean dirty state.
        clear_dirty(*root);
    }

    static void clear_dirty(Node& node) {
        node.clear_dirty(Dirty::All);
        for (const auto& child : node.children()) {
            clear_dirty(*child);
        }
    }

    [[nodiscard]] Node* find(Node& node, std::string_view kind) {
        if (node.element_kind() == kind) {
            return &node;
        }
        for (const auto& child : node.children()) {
            if (Node* const found = find(*child, kind)) {
                return found;
            }
        }
        return nullptr;
    }

    [[nodiscard]] tessera::ui::layout::LayoutInput build_layout_input(const Node& node) {
        tessera::ui::layout::LayoutInput input;
        input.id = node.id();
        input.style = styles.compute(node);
        const auto& paint = node.paint_data();
        if (paint && paint->kind == tessera::ui::PaintKind::Text) {
            input.text = paint->text;
        }
        for (const auto& child : node.children()) {
            input.children.push_back(build_layout_input(*child));
        }
        return input;
    }

    std::unique_ptr<RouteRecorder> root;
    demo::gen::Demo demo;
    DemoSettings styles;
    tessera::ui::layout::LayoutApplyResult layout_applied;
};

bool save_png(const std::string& path, const std::vector<std::uint8_t>& pixels) {
    return stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                          pixels.data(), static_cast<int>(kWidth) * 4) != 0;
}

bool compare_png(const std::string& path, const std::vector<std::uint8_t>& pixels,
                 const char* label) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (decoded == nullptr) {
        std::cerr << "generated_ui: failed to load golden " << path << "\n";
        return false;
    }
    const auto expected_size =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    const std::span<const std::uint8_t> expected(decoded, expected_size);
    const auto diff = compare_rgba(
        {kWidth, kHeight}, pixels,
        {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, expected, 0);
    stbi_image_free(decoded);
    if (diff.size_mismatch || diff.mismatched != 0) {
        std::cerr << "generated_ui: " << label << " golden mismatch: mismatched=" << diff.mismatched
                  << " max_delta=" << diff.max_channel_delta << " first=" << diff.first_mismatch
                  << "\n";
        return false;
    }
    return true;
}

std::uint64_t fnv1a(std::span<const std::uint8_t> pixels) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : pixels) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<std::uint8_t> render(Node& root, StyleProvider& styles, CpuCanvas& canvas,
                                 tessera::ui::text::GlyphRasterizer& rasterizer,
                                 TextShaper& shaper) {
    using tessera::rendering::rhi::rgba8;
    canvas.clear(rgba8(11, 18, 32));
    tessera::poc::batched2d::CpuPaintSink sink(canvas, &rasterizer, &shaper);
    paint_scene(root, styles, sink);
    const std::span<const std::uint8_t> pixels = canvas.pixels();
    return std::vector<std::uint8_t>(pixels.begin(), pixels.end());
}

bool route_is_w3c(const RouteRecorder& recorder, EventType type) {
    // The root is the only scene ancestor of the generated Button: it must see Capture before
    // Bubble, and the whole route has exactly those two entries for the event.
    std::vector<EventPhase> phases;
    for (const RouteRecorder::Entry& entry : recorder.route) {
        if (entry.type == type) {
            phases.push_back(entry.phase);
        }
    }
    if (phases.size() != 2) {
        return false;
    }
    return phases[0] == EventPhase::Capture && phases[1] == EventPhase::Bubble;
}

InputEvent pointer_event(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.pointer_id = 1;
    event.pointer_kind = PointerKind::Mouse;
    event.button = PointerButton::Left;
    event.is_primary = true;
    return event;
}

InputEvent edit_text_event(std::string_view text) {
    InputEvent event;
    event.type = InputEventType::EditText;
    const std::size_t size = std::min<std::size_t>(text.size(), event.text.size());
    std::copy_n(text.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    return event;
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
        std::cerr << "usage: generated_ui_tests [--write] <font.ttf> <baseline_dir>\n";
        return 2;
    }
    const std::string font_path = argv[argument];
    const std::string baseline_dir = argv[argument + 1];

    auto loaded = tessera::ui::text::FontFace::load_from_file(font_path);
    if (!loaded) {
        std::cerr << "generated_ui: failed to load font: " << loaded.error().message << "\n";
        return 2;
    }
    const tessera::ui::text::FontFace face = *loaded;
    tessera::ui::text::GlyphRasterizer rasterizer(face);
    HarfBuzzShaper shaper(face);

    // --- 1. Generated tree + Yoga layout + CPU paint -------------------------------------------
    Fixture fixture;
    if (!expect(fixture.layout_applied.applied == 5, "layout applied to every generated node") ||
        !expect(fixture.layout_applied.missing == 0 && fixture.layout_applied.extra == 0,
                "layout covered the tree exactly")) {
        return 3;
    }
    Node* const card = fixture.find(*fixture.root, "View");
    Node* const counter = fixture.find(*fixture.root, "Text");
    Node* const button = fixture.find(*fixture.root, "Button");
    Node* const text_input = fixture.find(*fixture.root, "TextInput");
    if (!expect(card != nullptr && counter != nullptr && button != nullptr && text_input != nullptr,
                "generated kinds are present")) {
        return 3;
    }
    if (!expect(counter->world_bounds().width > 0.0f, "text node laid out with a measured size") ||
        !expect(button->world_bounds().width == 96.0f, "button width from the style sheet")) {
        return 3;
    }
    // Resolve the generated nodes the way automation would: the sample does not set
    // automationIds yet (documented codegen gap), so the demo resolves by element kind.
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
    const Rect button_bounds = button->world_bounds();
    const float click_x = button_bounds.x + button_bounds.width * 0.5f;
    const float click_y = button_bounds.y + button_bounds.height * 0.5f;
    const auto down = router.dispatch(*fixture.root,
                                      pointer_event(InputEventType::PointerDown, click_x, click_y));
    const auto up =
        router.dispatch(*fixture.root, pointer_event(InputEventType::PointerUp, click_x, click_y));
    if (!expect(down.dispatched && up.dispatched, "pointer click dispatched") ||
        !expect(fixture.demo.state_count_ == 1, "generated handler incremented the state cell") ||
        !expect(route_is_w3c(*fixture.root, EventType::PointerDown), "PointerDown route is W3C") ||
        !expect(route_is_w3c(*fixture.root, EventType::PointerUp), "PointerUp route is W3C")) {
        return 6;
    }
    if (!expect(!has_flag(counter->dirty(), Dirty::Paint),
                "the generated handler itself marks no dirty domain (codegen gap)")) {
        return 6;
    }

    // The missing regeneration step: a framework would write the state back into the Text node's
    // PaintData. The test applies exactly that interpolation and proves the repaint path.
    tessera::ui::PaintData updated;
    updated.text = "Count: " + std::to_string(fixture.demo.state_count_);
    counter->set_paint_data(std::move(updated));
    if (!expect(has_flag(counter->dirty(), Dirty::Paint) &&
                    has_flag(counter->dirty(), Dirty::Render),
                "state interpolation marks Paint and Render") ||
        !expect(!has_flag(counter->dirty(), Dirty::Layout),
                "text interpolation does not mark Layout (no reflow needed)")) {
        return 7;
    }

    DirtyPass dirty;
    dirty.collect(*fixture.root, Dirty::Paint | Dirty::Render);
    tessera::rendering::DamageAccumulator damage(
        tessera::rendering::DamageConfig{Extent2D{kWidth, kHeight}, 16});
    const auto damage_result = tessera::ui::layout::add_scene_damage(dirty, damage, 1.0f);
    const Rect text_bounds = counter->world_bounds();
    bool damaged = false;
    for (const Rect& rect : damage.rects()) {
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
                "generated on_name handler copied the committed text into state")) {
        return 10;
    }

    // The same committed text through the edit controller's buffer (the controller is the owner
    // that a future controlled TextInput would bind to; there is no generated value/onChange wiring
    // between the buffer and state yet - documented gap).
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
    if (!expect(typed_render == after,
                "typing does not repaint: no controlled value -> PaintData wiring (documented)")) {
        return 11;
    }

    // --- 4. Determinism: rebuild the whole generated UI and reproduce the frames ----------------
    Fixture replay;
    Node* replay_counter = replay.find(*replay.root, "Text");
    Node* replay_button = replay.find(*replay.root, "Button");
    const Rect replay_bounds = replay_button->world_bounds();
    InputRouter replay_router;
    replay_router.dispatch(*replay.root,
                           pointer_event(InputEventType::PointerDown,
                                         replay_bounds.x + replay_bounds.width * 0.5f,
                                         replay_bounds.y + replay_bounds.height * 0.5f));
    replay_router.dispatch(*replay.root,
                           pointer_event(InputEventType::PointerUp,
                                         replay_bounds.x + replay_bounds.width * 0.5f,
                                         replay_bounds.y + replay_bounds.height * 0.5f));
    tessera::ui::PaintData replay_updated;
    replay_updated.text = "Count: " + std::to_string(replay.demo.state_count_);
    replay_counter->set_paint_data(std::move(replay_updated));
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
