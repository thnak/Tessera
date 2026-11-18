// SPDX-License-Identifier: MIT
//
// Per-run LCD eligibility at the paint seam (display-aware-rendering.md section 3.4.1, step 3).
// The window grants a permission (`PaintContext::lcd_text`); the paint pass narrows it per node:
// a run is marked `TextDraw::lcd` only when its node paints fully opaque, under a translate-only
// matrix, with no interpolation override. The mark must survive the paint cache's record/replay,
// and a record made under the other permission must never replay.
//
// Deterministic: no clock, fonts, network or GPU.

#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintCache;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_lcd_eligibility: " << message << "\n";
    }
    return condition;
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

// Records every text run's text and LCD mark; `transforms` flips `supports_transform()` so both
// the framed path (the sink applies the matrix) and the flattened path (an axis-aligned box) run.
class RecordingSink final : public PaintSink {
public:
    explicit RecordingSink(bool transforms = false)
        : transforms_(transforms) {}

    void fill_rect(const Rect&, const Color&) override {}
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    [[nodiscard]] bool supports_transform() const override {
        return transforms_;
    }
    void push_transform(const Mat3&) override {}
    void draw_text(const TextDraw& run) override {
        runs.emplace_back(std::string(run.text), run.lcd);
    }

    [[nodiscard]] int lcd_of(const std::string& text) const {
        for (const auto& [name, lcd] : runs) {
            if (name == text) {
                return lcd ? 1 : 0;
            }
        }
        return -1;
    }

    std::vector<std::pair<std::string, bool>> runs;

private:
    bool transforms_ = false;
};

void set_text(Node& node, std::string text) {
    PaintData data;
    data.text = std::move(text);
    node.set_paint_data(std::move(data));
}

ComputedStyle opacity(const char* value) {
    ComputedStyle style;
    style.set("opacity", value);
    return style;
}

PaintContext granted() {
    PaintContext context;
    context.lcd_text = true;
    return context;
}

bool permission_is_the_precondition() {
    Node root(1);
    root.set_bounds({0, 0, 200, 100});
    set_text(root, "plain");
    StubStyles styles;

    RecordingSink off;
    paint_scene(root, styles, off, PaintContext{});
    RecordingSink on;
    paint_scene(root, styles, on, granted());
    return expect(off.lcd_of("plain") == 0, "no window permission leaves every run grayscale") &&
           expect(on.lcd_of("plain") == 1, "an opaque, untransformed run is eligible");
}

bool a_faded_group_never_carries_fringes() {
    Node root(1);
    root.set_bounds({0, 0, 200, 100});
    Node& faded = root.add_child(std::make_unique<Node>(2));
    faded.set_bounds({0, 0, 100, 20});
    Node& inner = faded.add_child(std::make_unique<Node>(3));
    inner.set_bounds({0, 0, 50, 20});
    Node& solid = root.add_child(std::make_unique<Node>(4));
    solid.set_bounds({0, 40, 100, 20});
    set_text(faded, "faded");
    set_text(inner, "inner");
    set_text(solid, "solid");
    StubStyles styles;
    styles.set(2, opacity("0.5"));

    RecordingSink sink;
    paint_scene(root, styles, sink, granted());
    return expect(sink.lcd_of("faded") == 0, "a node at opacity 0.5 is grayscale") &&
           expect(sink.lcd_of("inner") == 0, "a child inherits its ancestor's fade") &&
           expect(sink.lcd_of("solid") == 1, "a sibling outside the fade stays eligible");
}

bool only_a_translate_only_matrix_is_eligible() {
    for (const bool framed : {false, true}) {
        Node root(1);
        root.set_bounds({0, 0, 200, 100});
        Node& rotated = root.add_child(std::make_unique<Node>(2));
        rotated.set_bounds({0, 0, 100, 20});
        rotated.set_transform(Mat3::rotation(0.3f));
        Node& scaled = root.add_child(std::make_unique<Node>(3));
        scaled.set_bounds({0, 40, 100, 20});
        scaled.set_transform(Mat3::scale(2.0f, 2.0f));
        Node& moved = root.add_child(std::make_unique<Node>(4));
        moved.set_bounds({0, 80, 100, 20});
        moved.set_transform(Mat3::translation(7.0f, 3.0f));
        set_text(rotated, "rotated");
        set_text(scaled, "scaled");
        set_text(moved, "moved");
        StubStyles styles;

        RecordingSink sink(framed);
        paint_scene(root, styles, sink, granted());
        const char* mode = framed ? " (sink applies the matrix)" : " (flattened box)";
        if (!expect(sink.lcd_of("rotated") == 0,
                    (std::string("rotation is grayscale") + mode).c_str()) ||
            !expect(sink.lcd_of("scaled") == 0,
                    (std::string("scale is grayscale") + mode).c_str())) {
            return false;
        }
        // A translation that a sink applies opens a frame, which the rule treats conservatively; a
        // flattened translation is part of the world matrix and stays translate-only.
        if (!framed && !expect(sink.lcd_of("moved") == 1, "a pure translation stays eligible")) {
            return false;
        }
    }
    return true;
}

bool the_mark_survives_the_cache_and_follows_the_permission() {
    Node root(1);
    root.set_bounds({0, 0, 200, 100});
    set_text(root, "cached");
    StubStyles styles;
    PaintCache cache;

    PaintContext context = granted();
    context.paint_cache = &cache;
    RecordingSink cold;
    paint_scene(root, styles, cold, context);
    root.clear_dirty(tessera::ui::Dirty::All);
    RecordingSink warm;
    paint_scene(root, styles, warm, context);
    const auto after_warm = cache.stats();

    // The permission flips (a policy change, e.g. the window moved to a translucent presentation):
    // the record made under the old value must miss, not replay a stale mark.
    context.lcd_text = false;
    RecordingSink revoked;
    paint_scene(root, styles, revoked, context);

    return expect(cold.lcd_of("cached") == 1, "the cold paint marks the run") &&
           expect(after_warm.hits > 0 && warm.lcd_of("cached") == 1,
                  "a cache hit replays the LCD mark") &&
           expect(revoked.lcd_of("cached") == 0,
                  "a record made under the other permission never replays");
}

} // namespace

int main() {
    bool ok = true;
    ok &= permission_is_the_precondition();
    ok &= a_faded_group_never_carries_fringes();
    ok &= only_a_translate_only_matrix_is_eligible();
    ok &= the_mark_survives_the_cache_and_follows_the_permission();
    return ok ? 0 : 1;
}
