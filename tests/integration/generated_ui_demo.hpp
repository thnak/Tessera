// SPDX-License-Identifier: MIT
//
// Fixtures for the headless generated-UI integration demo (task-56): the deterministic demo style
// sheet, the W3C route recorder, the generated component fixture (build + Yoga layout), a local
// CPU paint sink, and PNG golden helpers. Kept in a header so both the demo flow and its assertions
// stay small, and because the shared PoC CPU sink is owned by the assets lane and still changing.

#pragma once

#include "Demo.hpp"
#include "stb_image.h"
#include "stb_image_write.h"
#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace generated_ui_demo {

using tessera::rendering::golden::compare_rgba;
using tessera::rendering::rhi::Rect;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::Dirty;
using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PointerButton;
using tessera::ui::PointerKind;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::TextShaper;

inline constexpr std::uint32_t kWidth = 320;
inline constexpr std::uint32_t kHeight = 200;
inline constexpr NodeId kRootId = 900;

// Deterministic demo styles: the generated nodes carry element kinds/class names only (inline
// style attachment is a documented codegen gap), so the demo supplies the style-sheet values the
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

    struct Entry {
        EventType type = EventType::PointerDown;
        EventPhase phase = EventPhase::Target;
    };

    void on_event(Event& event) override {
        route.push_back({event.type, event.phase});
    }

    std::vector<Entry> route;
};

// Local CPU paint sink: solid rects with clipping and glyph-coverage text on the deterministic
// canvas. The shared PoC sink lives in tools/poc/batched2d and is owned/changing with the assets
// lane, so the demo keeps its own minimal copy (rects + text only; the demo scene has no rounded
// rects or vector payloads). Moving this into tessera_software is the follow-up when the CPU
// vector golden lands.
class DemoCpuSink final : public PaintSink {
public:
    DemoCpuSink(CpuCanvas& canvas, GlyphRasterizer& rasterizer, TextShaper& shaper)
        : canvas_(&canvas),
          rasterizer_(&rasterizer),
          shaper_(&shaper),
          clip_{0.0f, 0.0f, static_cast<float>(canvas.width()),
                static_cast<float>(canvas.height())} {}

    void fill_rect(const Rect& rect, const tessera::rendering::rhi::Color& color) override {
        const Rect clipped = intersect(clip_, rect);
        if (clipped.width > 0.0f && clipped.height > 0.0f) {
            canvas_->fill_rect(clipped, color);
        }
    }

    void fill_rounded_rect(const Rect& rect, const tessera::rendering::rhi::Color& color,
                           float /*radius*/) override {
        // The demo scene uses no rounded rects; bounds-only approximation documented.
        fill_rect(rect, color);
    }

    void push_clip(const tessera::ui::paint::RoundedClip& clip) override {
        stack_.push_back(clip_);
        clip_ = intersect(clip_, clip.rect);
    }

    void pop_clip() override {
        if (!stack_.empty()) {
            clip_ = stack_.back();
            stack_.pop_back();
        }
    }

    void draw_text(const tessera::ui::paint::TextDraw& run) override {
        if (run.text.empty()) {
            return;
        }
        tessera::ui::text::TextStyle style;
        style.font_size = run.font_size;
        style.font_weight = run.font_weight;
        const tessera::ui::text::ShapedRun shaped = shaper_->shape(run.text, style);
        const float size = std::clamp(run.font_size, 1.0f, 4096.0f);
        const auto pixel_size = static_cast<std::uint16_t>(std::lround(size));
        const float baseline = run.rect.y + shaped.ascent;
        float pen_x = run.rect.x;
        for (const tessera::ui::text::Glyph& glyph : shaped.glyphs) {
            const tessera::ui::text::GlyphBitmap bitmap =
                rasterizer_->rasterize_glyph(glyph.id, pixel_size);
            if (!bitmap.coverage.empty()) {
                const auto left = static_cast<std::int32_t>(
                    std::lround(pen_x + glyph.x_offset + static_cast<float>(bitmap.bearing_x)));
                const auto top = static_cast<std::int32_t>(
                    std::lround(baseline - glyph.y_offset - static_cast<float>(bitmap.bearing_y)));
                canvas_->blend_coverage(left, top, bitmap.width, bitmap.height, bitmap.coverage,
                                        run.color, &clip_);
            }
            pen_x += glyph.advance;
        }
    }

private:
    static Rect intersect(const Rect& lhs, const Rect& rhs) {
        const float left = std::max(lhs.x, rhs.x);
        const float top = std::max(lhs.y, rhs.y);
        const float right = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
        const float bottom = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
        if (left >= right || top >= bottom) {
            return {0.0f, 0.0f, 0.0f, 0.0f};
        }
        return {left, top, right - left, bottom - top};
    }

    CpuCanvas* canvas_ = nullptr;
    GlyphRasterizer* rasterizer_ = nullptr;
    TextShaper* shaper_ = nullptr;
    Rect clip_;
    std::vector<Rect> stack_;
};

// Generated component + scene root + the Yoga layout result. The root is the only hand-authored
// node (the Recorder used for the route log); every visual node comes from `demo::gen::Demo`.
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
        layout_applied = tessera::ui::layout::apply_layout(result, *root);
        // apply_layout writes bounds (which marks Paint|Render); a frame would clear those after
        // painting, so interaction assertions start from a clean dirty state.
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

inline bool save_png(const std::string& path, const std::vector<std::uint8_t>& pixels) {
    return stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                          pixels.data(), static_cast<int>(kWidth) * 4) != 0;
}

inline bool compare_png(const std::string& path, const std::vector<std::uint8_t>& pixels,
                        const char* label) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (decoded == nullptr) {
        std::fprintf(stderr, "generated_ui: failed to load golden %s\n", path.c_str());
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
        std::fprintf(stderr,
                     "generated_ui: %s golden mismatch: mismatched=%zu max_delta=%u first=%zu\n",
                     label, diff.mismatched, diff.max_channel_delta, diff.first_mismatch);
        return false;
    }
    return true;
}

inline std::uint64_t fnv1a(std::span<const std::uint8_t> pixels) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : pixels) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::vector<std::uint8_t> render(Node& root, StyleProvider& styles, CpuCanvas& canvas,
                                        GlyphRasterizer& rasterizer, TextShaper& shaper) {
    canvas.clear(tessera::rendering::rhi::rgba8(11, 18, 32));
    DemoCpuSink sink(canvas, rasterizer, shaper);
    paint_scene(root, styles, sink);
    const std::span<const std::uint8_t> pixels = canvas.pixels();
    return std::vector<std::uint8_t>(pixels.begin(), pixels.end());
}

inline bool route_is_w3c(const RouteRecorder& recorder, EventType type) {
    // The root is the only scene ancestor of the generated nodes: it must see Capture before
    // Bubble, and the whole route has exactly those two entries for the event.
    std::vector<EventPhase> phases;
    for (const RouteRecorder::Entry& entry : recorder.route) {
        if (entry.type == type) {
            phases.push_back(entry.phase);
        }
    }
    return phases.size() == 2 && phases[0] == EventPhase::Capture &&
           phases[1] == EventPhase::Bubble;
}

inline InputEvent pointer_event(InputEventType type, float x, float y) {
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

inline InputEvent edit_text_event(std::string_view text) {
    InputEvent event;
    event.type = InputEventType::EditText;
    const std::size_t size = std::min<std::size_t>(text.size(), event.text.size());
    std::copy_n(text.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    return event;
}

} // namespace generated_ui_demo
