// SPDX-License-Identifier: MIT
//
// Paint-pass glass behaviour (issue #244, docs/specs/glass-v1.md sections 2, 3 and 4):
// segmentation of the node list at a glass node (the backdrop draw lands exactly between the paint
// order before the node and the node's own box visuals), the radius clamp, the webkit alias,
// high-contrast suppression, paint-cache replay of a recorded backdrop, and the CPU reference
// sink's documented "unsupported" fallback of section 9.

#include "cpu_paint_sink.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;

using tessera::poc::batched2d::CpuPaintSink;
using tessera::rendering::gpu::GlassLimits;
using tessera::rendering::rhi::Rect;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::BackdropDraw;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintCache;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glass_paint: " << message << "\n";
    }
    return condition;
}

bool same_rect(const Rect& lhs, const Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }

    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

// Records the sink calls in emission order: 1 = fill, 2 = backdrop.
struct Call {
    int kind = 0;
    Rect rect;
    BackdropDraw backdrop;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const rhi::Color& /*color*/) override {
        calls.push_back(Call{1, rect, {}});
    }
    void fill_rounded_rect(const Rect& rect, const rhi::Color& /*color*/,
                           float /*radius*/) override {
        calls.push_back(Call{1, rect, {}});
    }
    [[nodiscard]] bool supports_backdrop() const override {
        return true;
    }
    void draw_backdrop(const BackdropDraw& draw) override {
        calls.push_back(Call{2, draw.rect, draw});
    }

    std::vector<Call> calls;
};

// root(1) background, under(2) background, glass(3) with child(4). The glass node's backdrop must
// land after the two fills before it and before its own background.
struct GlassScene {
    Node root{1};
    StubStyles styles;
    GlassLimits limits;

    GlassScene() {
        root.set_bounds({0.0f, 0.0f, 200.0f, 200.0f});
        auto under = std::make_unique<Node>(2);
        under->set_bounds({10.0f, 10.0f, 180.0f, 20.0f});
        root.add_child(std::move(under));
        auto glass = std::make_unique<Node>(3);
        glass->set_bounds({40.0f, 60.0f, 100.0f, 60.0f});
        auto child = std::make_unique<Node>(4);
        child->set_bounds({10.0f, 10.0f, 20.0f, 20.0f});
        glass->add_child(std::move(child));
        root.add_child(std::move(glass));

        styles.set(1, styled({{"background-color", "#202020"}}));
        styles.set(2, styled({{"background-color", "#ff0000"}}));
        styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"},
                              {"backdrop-filter", "blur(8px)"}}));
        styles.set(4, styled({{"background-color", "#0000ff"}}));
        clear_tree(root);
    }

    PaintContext context() {
        PaintContext ctx;
        ctx.glass = limits;
        return ctx;
    }
};

bool backdrop_placement() {
    GlassScene scene;
    RecordingSink sink;
    const PaintStats stats = paint_scene(scene.root, scene.styles, sink, scene.context());
    bool ok = expect(stats.backdrops == 1, "one backdrop draw per glass node");
    ok = expect(sink.calls.size() == 5, "root, under, backdrop, glass background, child") && ok;
    if (sink.calls.size() != 5) {
        return false;
    }
    const int expected[] = {1, 1, 2, 1, 1};
    for (std::size_t index = 0; index < 5; ++index) {
        ok = expect(sink.calls[index].kind == expected[index],
                    "the backdrop is emitted after earlier paint and before the node's own box") &&
             ok;
    }
    const BackdropDraw& draw = sink.calls[2].backdrop;
    ok = expect(same_rect(draw.rect, Rect{40.0f, 60.0f, 100.0f, 60.0f}),
                "the backdrop covers the node's border box") &&
         ok;
    ok = expect(same_rect(draw.clip.rect, draw.rect), "the composite clip is the node's box") && ok;
    ok = expect(draw.sigma == 8.0f, "the sigma is the declared blur length") && ok;
    ok = expect(draw.opacity == 1.0f, "an opaque node composites the backdrop fully") && ok;
    ok = expect(draw.device_scale == 1.0f, "the default device scale is 1") && ok;
    return ok;
}

bool clamp_and_fold() {
    GlassScene scene;
    scene.styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"},
                                {"backdrop-filter", "blur(200px) saturate(150%) brightness(80%) "
                                                    "contrast(2)"},
                                {"opacity", "0.5"}}));
    scene.limits.max_blur_radius = 20.0f;
    RecordingSink sink;
    const PaintStats stats = paint_scene(scene.root, scene.styles, sink, scene.context());
    bool ok = expect(stats.backdrops == 1, "a long declaration still emits one backdrop");
    const BackdropDraw* draw = nullptr;
    for (const Call& call : sink.calls) {
        if (call.kind == 2) {
            draw = &call.backdrop;
        }
    }
    ok = expect(draw != nullptr, "the backdrop draw exists") && ok;
    if (draw == nullptr) {
        return false;
    }
    ok = expect(draw->sigma == 20.0f, "the declared sigma clamps to glassMaxBlurRadius") && ok;
    ok = expect(draw->saturate == 1.5f && draw->brightness == 0.8f && draw->contrast == 2.0f,
                "color operators resolve") &&
         ok;
    ok = expect(draw->opacity == 0.5f, "node opacity folds into the composite") && ok;
    return ok;
}

bool webkit_alias_and_none() {
    GlassScene scene;
    scene.styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"},
                                {"-webkit-backdrop-filter", "blur(2px)"}}));
    RecordingSink sink;
    PaintStats stats = paint_scene(scene.root, scene.styles, sink, scene.context());
    bool ok = expect(stats.backdrops == 1, "the -webkit- alias emits a backdrop");
    for (const Call& call : sink.calls) {
        if (call.kind == 2) {
            ok = expect(call.backdrop.sigma == 2.0f, "the alias value is parsed") && ok;
        }
    }
    // `none` and blur(0) are pass-throughs.
    for (const char* value : {"none", "blur(0)"}) {
        GlassScene plain;
        plain.styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"},
                                    {"backdrop-filter", value}}));
        RecordingSink none_sink;
        const PaintStats none_stats =
            paint_scene(plain.root, plain.styles, none_sink, plain.context());
        ok = expect(none_stats.backdrops == 0, "`none`/blur(0) emit no backdrop") && ok;
    }
    return ok;
}

bool high_contrast_ignores_backdrop_filter() {
    GlassScene scene;
    RecordingSink ignored;
    PaintContext high_contrast = scene.context();
    high_contrast.high_contrast = true;
    const PaintStats stats = paint_scene(scene.root, scene.styles, ignored, high_contrast);
    bool ok = expect(stats.backdrops == 0, "high contrast emits no backdrop");

    // The stylesheet fallback is what shows: the output is exactly the same scene without the
    // declaration (the node's own background paints normally).
    GlassScene fallback;
    fallback.styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"}}));
    RecordingSink plain;
    paint_scene(fallback.root, fallback.styles, plain, fallback.context());
    ok = expect(ignored.calls.size() == plain.calls.size(),
                "high contrast paints exactly the no-filter scene") &&
         ok;
    for (std::size_t index = 0; ok && index < ignored.calls.size(); ++index) {
        ok = expect(ignored.calls[index].kind == plain.calls[index].kind &&
                        same_rect(ignored.calls[index].rect, plain.calls[index].rect),
                    "each high-contrast draw matches the fallback") &&
             ok;
    }
    return ok;
}

bool paint_cache_replays_backdrop() {
    GlassScene scene;
    PaintCache cache;
    PaintContext context = scene.context();
    context.paint_cache = &cache;

    RecordingSink first;
    paint_scene(scene.root, scene.styles, first, context);
    RecordingSink second;
    const PaintStats stats = paint_scene(scene.root, scene.styles, second, context);
    bool ok = expect(stats.backdrops == 1, "a cache hit replays the backdrop draw");
    ok = expect(cache.stats().hits > 0, "the record was replayed") && ok;
    ok = expect(first.calls.size() == second.calls.size(), "replay emits the same call sequence") &&
         ok;

    // High contrast is part of the cache key: the flip re-records instead of replaying a backdrop.
    PaintContext high_contrast = context;
    high_contrast.high_contrast = true;
    RecordingSink third;
    paint_scene(scene.root, scene.styles, third, high_contrast);
    ok = expect(cache.stats().misses > 0, "a high-contrast flip misses the record") && ok;
    std::size_t backdrops = 0;
    for (const Call& call : third.calls) {
        backdrops += call.kind == 2 ? 1 : 0;
    }
    ok = expect(backdrops == 0, "the re-recorded paint has no backdrop") && ok;
    return ok;
}

// The CPU reference sink cannot read the frame back, so glass is explicitly unsupported there
// (glass-v1 section 9): it counts the draw and keeps the stylesheet fallback.
bool cpu_sink_reports_unsupported_and_falls_back() {
    GlassScene scene;
    constexpr std::uint32_t kWidth = 200;
    constexpr std::uint32_t kHeight = 200;
    CpuCanvas canvas(kWidth, kHeight);
    canvas.clear({1.0f, 1.0f, 1.0f, 1.0f});
    CpuPaintSink sink(canvas);
    const PaintStats stats = paint_scene(scene.root, scene.styles, sink, scene.context());
    bool ok = expect(stats.backdrops == 1, "the paint pass emitted the glass draw");
    ok = expect(sink.glass_unsupported() == 1, "the CPU sink reports the unsupported glass draw") &&
         ok;

    GlassScene fallback;
    fallback.styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"}}));
    CpuCanvas fallback_canvas(kWidth, kHeight);
    fallback_canvas.clear({1.0f, 1.0f, 1.0f, 1.0f});
    CpuPaintSink fallback_sink(fallback_canvas);
    paint_scene(fallback.root, fallback.styles, fallback_sink, fallback.context());
    ok = expect(fallback_sink.glass_unsupported() == 0, "the fallback scene has no glass draw") &&
         ok;
    ok = expect(std::equal(canvas.pixels().begin(), canvas.pixels().end(),
                           fallback_canvas.pixels().begin()),
                "the CPU fallback pixels are exactly the no-filter scene") &&
         ok;
    return ok;
}

bool tree_without_glass_is_unchanged() {
    GlassScene scene;
    scene.styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"}}));
    RecordingSink sink;
    const PaintStats stats = paint_scene(scene.root, scene.styles, sink, scene.context());
    bool ok = expect(stats.backdrops == 0, "a tree without a glass node emits no backdrop");
    ok = expect(stats.backgrounds == 4, "the background draws are exactly the same") && ok;
    ok = expect(sink.calls.size() == 4, "the sink sees only the ordinary fills") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = backdrop_placement();
    ok = clamp_and_fold() && ok;
    ok = webkit_alias_and_none() && ok;
    ok = high_contrast_ignores_backdrop_filter() && ok;
    ok = paint_cache_replays_backdrop() && ok;
    ok = cpu_sink_reports_unsupported_and_falls_back() && ok;
    ok = tree_without_glass_is_unchanged() && ok;
    if (!ok) {
        std::cerr << "glass_paint: FAILED\n";
        return 1;
    }
    std::cout << "glass_paint: ok\n";
    return 0;
}
