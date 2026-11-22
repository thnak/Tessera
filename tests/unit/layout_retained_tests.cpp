// SPDX-License-Identifier: MIT
//
// The acceptance gate for the retained Yoga tree (issue #735, slice D): a randomized edit sequence
// over one tree, where every frame's retained result must equal the from-scratch `layout_cold`
// result box for box. The retained tree is only worth having if it produces the same boxes; this
// test is what proves it, and it is the reason the slice is safe to land.
//
// The edit sequence covers every way a frame can change the tree: a size change, a CSS `order`
// change (which reorders Yoga children), `display: none`/`display: block` flips, text changes, a
// child added and removed, a grid container whose tracks change, and a nested flex subtree. The
// generator is a fixed-seed LCG, so a failure is reproducible from the printed frame number.

#include "layout_cold.hpp"
#include "layout_retained.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::layout::detail::layout_cold;
using tessera::ui::layout::detail::RetainedTree;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "retained: " << message << "\n";
    }
    return condition;
}

// A fixed-seed LCG: deterministic, so a failing frame is reproducible, and no `<random>` engine
// whose output differs between standard libraries.
class Rng {
public:
    explicit Rng(std::uint64_t seed)
        : state_(seed) {}

    std::uint32_t next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33);
    }

    // A value in [0, bound).
    std::uint32_t below(std::uint32_t bound) {
        return bound == 0 ? 0 : next() % bound;
    }

private:
    std::uint64_t state_;
};

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

LayoutInput node(std::uint64_t id, ComputedStyle computed, std::vector<LayoutInput> children = {}) {
    LayoutInput result;
    result.id = id;
    result.style = std::move(computed);
    result.children = std::move(children);
    return result;
}

LayoutInput text_node(std::uint64_t id, ComputedStyle computed, std::string text) {
    LayoutInput result;
    result.id = id;
    result.style = std::move(computed);
    result.text = std::move(text);
    return result;
}

// The tree the edit sequence mutates. Ids are stable, so the retained tree can match nodes across
// frames; the shape is a page with a flex row of cards, one of which holds a nested flex column and
// one a grid container.
struct Scene {
    LayoutInput root;

    explicit Scene() {
        std::vector<LayoutInput> cards;
        cards.push_back(node(10,
                             style({{"display", "flex"},
                                    {"flex-direction", "column"},
                                    {"width", "120px"},
                                    {"height", "40px"},
                                    {"padding-top", "4px"},
                                    {"padding-right", "4px"},
                                    {"padding-bottom", "4px"},
                                    {"padding-left", "4px"},
                                    {"margin-top", "2px"},
                                    {"margin-right", "2px"},
                                    {"margin-bottom", "2px"},
                                    {"margin-left", "2px"}}),
                             {text_node(11, style({{"font-size", "14px"}}), "alpha"),
                              text_node(12, style({{"font-size", "14px"}}), "beta")}));
        cards.push_back(node(20,
                             style({{"display", "flex"},
                                    {"flex-direction", "row"},
                                    {"width", "120px"},
                                    {"height", "40px"}}),
                             {node(21, style({{"width", "50px"}, {"height", "20px"}})),
                              node(22, style({{"width", "50px"}, {"height", "20px"}}))}));
        cards.push_back(node(30,
                             style({{"display", "grid"},
                                    {"width", "200px"},
                                    {"height", "80px"},
                                    {"grid-template-columns", "100px 100px"},
                                    {"grid-template-rows", "40px 40px"}}),
                             {node(31, {}), node(32, {}), node(33, {}), node(34, {})}));
        cards.push_back(node(40, style({{"width", "120px"}, {"height", "40px"}}),
                             {text_node(41, style({{"font-size", "12px"}}), "gamma")}));
        root = node(1,
                    style({{"display", "flex"},
                           {"flex-direction", "row"},
                           {"width", "600px"},
                           {"height", "200px"}}),
                    std::move(cards));
    }
};

LayoutInput* find(LayoutInput& root, std::uint64_t id) {
    if (root.id == id) {
        return &root;
    }
    for (LayoutInput& child : root.children) {
        if (LayoutInput* found = find(child, id)) {
            return found;
        }
    }
    return nullptr;
}

// One edit. Every branch is a change the retained tree has to notice: a style digest change, a
// reorder, a display flip, a text change, a child count change, or a grid track change.
void edit(Scene& scene, Rng& rng, std::uint32_t frame) {
    switch (rng.below(9)) {
    case 0: {
        // A size change on one card: the case the slice exists for.
        LayoutInput* card = find(scene.root, 10);
        card->style.set("width", (frame % 2) == 0 ? "121px" : "123px");
        break;
    }
    case 1: {
        // A CSS `order` change: the retained tree must reorder the Yoga children.
        LayoutInput* card = find(scene.root, 20);
        card->style.set("order", std::to_string(static_cast<int>(rng.below(5)) - 2));
        break;
    }
    case 2: {
        // `display: none` on and off, on a card with children.
        LayoutInput* card = find(scene.root, 40);
        const bool hidden = card->style.find("display") != nullptr;
        if (hidden) {
            card->style.erase("display");
        } else {
            card->style.set("display", "none");
        }
        break;
    }
    case 3: {
        // A text change, which must re-measure the leaf.
        LayoutInput* label = find(scene.root, 11);
        label->text = (frame % 2) == 0 ? "alpha" : "alpha beta gamma";
        break;
    }
    case 4: {
        // A child added and removed: the child count changes.
        LayoutInput* card = find(scene.root, 20);
        if (card->children.size() > 2) {
            card->children.pop_back();
        } else {
            card->children.push_back(node(23, style({{"width", "30px"}, {"height", "20px"}})));
        }
        break;
    }
    case 5: {
        // A grid track change, which re-runs the grid engine over the owned subtree input.
        LayoutInput* grid = find(scene.root, 30);
        grid->style.set("grid-template-columns", (frame % 2) == 0 ? "100px 100px" : "60px 140px");
        break;
    }
    case 6: {
        // A nested flex direction flip.
        LayoutInput* card = find(scene.root, 10);
        card->style.set("flex-direction", (frame % 2) == 0 ? "column" : "row");
        break;
    }
    case 7: {
        // A longhand *removed*, not just changed. `apply_style` only writes the longhands the style
        // has, so a retained node that patched its style in place would keep the old `width`; this
        // is the case the pristine reset exists for.
        LayoutInput* card = find(scene.root, 40);
        if (card->style.find("width") != nullptr) {
            card->style.erase("width");
        } else {
            card->style.set("width", "120px");
        }
        break;
    }
    default: {
        // A padding change, which moves the text-fit fact's content box.
        LayoutInput* card = find(scene.root, 40);
        card->style.set("padding-left", (frame % 2) == 0 ? "0px" : "8px");
        break;
    }
    }
}

bool same_boxes(const LayoutResult& retained, const LayoutResult& cold, std::uint32_t frame) {
    if (retained.boxes.size() != cold.boxes.size()) {
        std::cerr << "  frame " << frame << ": box count " << retained.boxes.size() << " vs "
                  << cold.boxes.size() << "\n";
        return false;
    }
    for (std::size_t i = 0; i < retained.boxes.size(); ++i) {
        const LayoutBox& lhs = retained.boxes[i];
        const LayoutBox& rhs = cold.boxes[i];
        const bool same = lhs.id == rhs.id && std::abs(lhs.bounds.x - rhs.bounds.x) <= 0.01f &&
                          std::abs(lhs.bounds.y - rhs.bounds.y) <= 0.01f &&
                          std::abs(lhs.bounds.width - rhs.bounds.width) <= 0.01f &&
                          std::abs(lhs.bounds.height - rhs.bounds.height) <= 0.01f &&
                          lhs.content_insets == rhs.content_insets && lhs.text_fit == rhs.text_fit;
        if (!same) {
            std::cerr << "  frame " << frame << " box " << i << " id " << lhs.id << " vs " << rhs.id
                      << ": retained " << lhs.bounds.x << "," << lhs.bounds.y << " "
                      << lhs.bounds.width << "x" << lhs.bounds.height << " cold " << rhs.bounds.x
                      << "," << rhs.bounds.y << " " << rhs.bounds.width << "x" << rhs.bounds.height
                      << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    const LayoutViewport viewport{600.0f, 200.0f};
    const auto measurer = std::make_shared<tessera::ui::text::StubTextMeasurer>();
    tessera::ui::text::TextMeasureCache cache(measurer, 1024);
    tessera::ui::layout::detail::RetainedTree retained;
    Scene scene;
    Rng rng(0x7350dULL);

    constexpr std::uint32_t kFrames = 400;
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        if (frame > 0) {
            edit(scene, rng, frame);
        }
        const LayoutResult retained_result = retained.layout(scene.root, viewport, cache);
        const LayoutResult cold_result =
            layout_cold(scene.root, viewport, viewport.width, viewport.height, cache,
                        /*as_grid_item=*/false);
        if (!same_boxes(retained_result, cold_result, frame)) {
            return 1;
        }
    }

    // A steady frame must not re-apply any style: the digest gate is what makes the slice pay.
    const std::size_t before = retained.style_applications();
    for (int i = 0; i < 5; ++i) {
        (void)retained.layout(scene.root, viewport, cache);
    }
    if (!expect(retained.style_applications() == before, "a steady frame re-applied a style")) {
        return 1;
    }

    // A viewport change must re-apply every style but keep the tree.
    const std::size_t nodes_before = retained.node_count();
    const LayoutViewport resized{500.0f, 180.0f};
    const LayoutResult resized_result = retained.layout(scene.root, resized, cache);
    const LayoutResult resized_cold =
        layout_cold(scene.root, resized, resized.width, resized.height, cache,
                    /*as_grid_item=*/false);
    if (!same_boxes(resized_result, resized_cold, kFrames)) {
        return 1;
    }
    if (!expect(retained.node_count() == nodes_before, "a viewport change rebuilt the tree")) {
        return 1;
    }

    // A different root id must rebuild rather than reuse the old tree.
    Scene other;
    other.root.id = 99;
    const LayoutResult other_result = retained.layout(other.root, viewport, cache);
    const LayoutResult other_cold =
        layout_cold(other.root, viewport, viewport.width, viewport.height, cache,
                    /*as_grid_item=*/false);
    if (!same_boxes(other_result, other_cold, kFrames + 1)) {
        return 1;
    }

    // A negative viewport axis means "unconstrained", and the page adjustments must not apply: the
    // retained tree has to gate them exactly as the from-scratch path does, or a root that was a
    // page keeps its `min-height` and `width: 100%` after the constraint goes away.
    for (const LayoutViewport& unbounded :
         {LayoutViewport{-1.0f, 200.0f}, LayoutViewport{600.0f, -1.0f},
          LayoutViewport{-1.0f, -1.0f}}) {
        const LayoutResult retained_unbounded = retained.layout(scene.root, unbounded, cache);
        const LayoutResult cold_unbounded =
            layout_cold(scene.root, unbounded, unbounded.width, unbounded.height, cache,
                        /*as_grid_item=*/false);
        if (!same_boxes(retained_unbounded, cold_unbounded, kFrames + 2)) {
            return 1;
        }
    }

    // `display: contents` is skipped by Yoga's layoutable-children walk but still exists as a node,
    // and Yoga tracks how many such children a container has. A flip must not desynchronise that
    // count, so the retained tree has to go through insert/remove rather than `YGNodeSetChildren`.
    Scene contents;
    contents.root.children[1].style.set("display", "contents");
    for (int frame = 0; frame < 4; ++frame) {
        if (frame == 2) {
            contents.root.children[1].style.erase("display");
        }
        const LayoutResult retained_contents = retained.layout(contents.root, viewport, cache);
        const LayoutResult cold_contents =
            layout_cold(contents.root, viewport, viewport.width, viewport.height, cache,
                        /*as_grid_item=*/false);
        if (!same_boxes(retained_contents, cold_contents, kFrames + 3)) {
            return 1;
        }
    }

    std::cout << "retained: " << kFrames << " frames, " << retained.node_count() << " nodes, "
              << retained.style_applications() << " style applications, " << retained.rebuilds()
              << " rebuilds\n";
    return 0;
}
