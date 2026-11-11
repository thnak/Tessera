// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_scheduler.hpp"
#include "tessera/ui/layout/layout_signature.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <utility>

namespace {

using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutScheduler;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_signature: " << message << "\n";
    }
    return condition;
}

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

LayoutInput node(std::uint64_t id,
                 std::initializer_list<std::pair<const char*, const char*>> values) {
    LayoutInput result;
    result.id = id;
    result.style = style(values);
    return result;
}

LayoutInput make_scene() {
    LayoutInput root = node(1, {{"display", "flex"},
                                {"flex-direction", "column"},
                                {"width", "100%"},
                                {"height", "200px"},
                                {"padding-top", "10px"},
                                {"padding-right", "10px"},
                                {"padding-bottom", "10px"},
                                {"padding-left", "10px"},
                                {"row-gap", "4px"}});

    LayoutInput header =
        node(2, {{"width", "100%"}, {"height", "40px"}, {"background-color", "#202020"}});

    LayoutInput row = node(3, {{"display", "flex"},
                               {"flex-direction", "row"},
                               {"width", "100%"},
                               {"height", "70px"},
                               {"column-gap", "6px"}});
    row.children.push_back(node(4, {{"width", "60px"}, {"height", "50px"}}));
    LayoutInput label = node(
        5,
        {{"width", "80px"}, {"height", "50px"}, {"white-space", "nowrap"}, {"font-size", "14px"}});
    label.text = "hello";
    row.children.push_back(label);
    row.children.push_back(node(6, {{"width", "40px"}, {"height", "50px"}}));

    root.children.push_back(header);
    root.children.push_back(row);
    return root;
}

const LayoutBox* find_box(const LayoutResult& result, std::uint64_t id) {
    for (const LayoutBox& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

// Exact field comparison: the guard promises the same boxes a reflow would produce, not an
// approximate match.
bool same_result(const LayoutResult& lhs, const LayoutResult& rhs) {
    if (lhs.boxes.size() != rhs.boxes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.boxes.size(); ++i) {
        const LayoutBox& a = lhs.boxes[i];
        const LayoutBox& b = rhs.boxes[i];
        if (a.id != b.id || a.bounds.x != b.bounds.x || a.bounds.y != b.bounds.y ||
            a.bounds.width != b.bounds.width || a.bounds.height != b.bounds.height ||
            !(a.content_insets == b.content_insets)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    LayoutEngine engine;
    LayoutEngine fresh;
    LayoutScheduler scheduler(engine);

    LayoutInput scene = make_scene();
    LayoutViewport viewport;
    viewport.width = 320.0f;
    viewport.height = 200.0f;

    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (!expect(condition, message)) {
            ++failures;
        }
    };
    // Every guarded result must equal a fresh full reflow of the same inputs.
    const auto verify = [&]() {
        return same_result(scheduler.current(), fresh.layout(scene, viewport));
    };

    // 1. First update reflows and caches the signature.
    std::uint64_t signature = tessera::ui::layout::layout_signature(scene, viewport);
    (void)scheduler.update(scene, viewport, true);
    check(scheduler.layout_runs() == 1 && scheduler.cache_hits() == 0, "first update reflows once");
    check(scheduler.signature() == signature, "signature cached with the result");
    check(verify(), "first result equals a fresh reflow");

    // 2. A clean frame is served from the cache.
    std::size_t runs = scheduler.layout_runs();
    std::size_t hits = scheduler.cache_hits();
    (void)scheduler.update(scene, viewport, false);
    check(scheduler.layout_runs() == runs && scheduler.cache_hits() == hits + 1,
          "clean frame hits the cache");

    // 3. Paint-only longhands change nothing a box can depend on, even though the caller raised
    //    layout dirt (the case this guard exists for).
    scene.children[0].style.set("background-color", "#303030");
    scene.children[0].style.set("color", "#ffffff");
    scene.children[0].style.set("opacity", "0.5");
    scene.children[0].style.set("text-overflow", "ellipsis");
    scene.children[0].style.set("font-family", "serif");
    scene.children[1].style.set("border-top-color", "#ffffff");
    check(tessera::ui::layout::layout_signature(scene, viewport) == signature,
          "paint-only change keeps the signature");
    runs = scheduler.layout_runs();
    hits = scheduler.cache_hits();
    (void)scheduler.update(scene, viewport, true);
    check(scheduler.layout_runs() == runs, "spurious layout dirt produces zero reflows");
    check(scheduler.cache_hits() == hits + 1, "spurious dirt is counted as a cache hit");
    check(verify(), "paint-only reuse equals a fresh reflow");

    // 4. A text change is a layout input (it can change a measured leaf) and reflows exactly once.
    runs = scheduler.layout_runs();
    const std::uint64_t before_text = tessera::ui::layout::layout_signature(scene, viewport);
    scene.children[1].children[1].text = "hello wide world";
    check(tessera::ui::layout::layout_signature(scene, viewport) != before_text,
          "text is in the signature");
    (void)scheduler.update(scene, viewport, true);
    check(scheduler.layout_runs() == runs + 1, "text change reflows exactly once");
    check(verify(), "text change equals a fresh reflow");
    const LayoutBox* label = find_box(scheduler.current(), 5);
    check(label != nullptr && label->bounds.width == 80.0f && label->bounds.height == 50.0f,
          "fixed-size text leaf keeps its box across the text change");

    // 5. A real width change reflows exactly once and moves the box.
    runs = scheduler.layout_runs();
    scene.children[1].children[0].style.set("width", "90px");
    (void)scheduler.update(scene, viewport, true);
    check(scheduler.layout_runs() == runs + 1, "width change reflows exactly once");
    check(verify(), "width change equals a fresh reflow");
    const LayoutBox* grown = find_box(scheduler.current(), 4);
    check(grown != nullptr && grown->bounds.width == 90.0f, "width change reaches the box");

    // 6. Child order is part of the tree shape.
    runs = scheduler.layout_runs();
    const std::uint64_t before_reorder = tessera::ui::layout::layout_signature(scene, viewport);
    std::swap(scene.children[0], scene.children[1]);
    check(tessera::ui::layout::layout_signature(scene, viewport) != before_reorder,
          "child reorder changes the signature");
    (void)scheduler.update(scene, viewport, true);
    check(scheduler.layout_runs() == runs + 1, "child reorder reflows exactly once");
    check(verify(), "child reorder equals a fresh reflow");

    // 7. The viewport is part of the signature.
    runs = scheduler.layout_runs();
    viewport.width = 480.0f;
    (void)scheduler.update(scene, viewport, true);
    check(scheduler.layout_runs() == runs + 1, "viewport change reflows exactly once");
    check(verify(), "viewport change equals a fresh reflow");

    // 8. An explicit invalidate always forces exactly one reflow.
    scheduler.invalidate();
    check(!scheduler.valid() && !scheduler.has_signature(), "invalidate clears the signature");
    runs = scheduler.layout_runs();
    (void)scheduler.update(scene, viewport, false);
    check(scheduler.layout_runs() == runs + 1, "invalidate forces exactly one reflow");
    check(verify(), "post-invalidate result equals a fresh reflow");

    // 9. A subtree patch is an approximation, so it drops the signature: the next update reflows.
    check(scheduler.relayout_subtree(scene, 5, viewport), "subtree patch applies");
    check(!scheduler.has_signature(), "subtree patch drops the signature");
    runs = scheduler.layout_runs();
    (void)scheduler.update(scene, viewport, false);
    check(scheduler.layout_runs() == runs + 1, "update after a patch reflows");
    check(verify(), "post-patch reflow equals a fresh reflow");

    // The signature itself: inclusion list, exclusions and determinism.
    const LayoutViewport probe_viewport;
    LayoutInput probe = node(42, {{"width", "100px"}, {"height", "25px"}});
    probe.text = "x";
    const std::uint64_t probe_signature =
        tessera::ui::layout::layout_signature(probe, probe_viewport);
    check(tessera::ui::layout::layout_signature(probe, probe_viewport) == probe_signature,
          "signature is deterministic across calls");

    const auto signature_after = [&probe, &probe_viewport](const auto& mutate) {
        LayoutInput copy = probe;
        mutate(copy);
        return tessera::ui::layout::layout_signature(copy, probe_viewport);
    };

    struct LonghandProbe {
        const char* property;
        const char* value;
        bool included;
    };
    // Every entry is classified: a layout longhand must move the signature, a paint-only one must
    // not. `font-family` is paint-only because the measure seam takes a `text::TextStyle` (no
    // family); that exclusion is the one to revisit if the text seam grows a family field.
    constexpr LonghandProbe kLonghands[] = {
        {"background-color", "#abcdef", false},
        {"color", "#123456", false},
        {"opacity", "0.25", false},
        {"visibility", "hidden", false},
        {"transform", "rotate(4deg)", false},
        {"border-radius", "4px", false},
        {"box-shadow", "0 1px 2px #000", false},
        {"text-overflow", "ellipsis", false},
        {"font-family", "serif", false},
        {"z-index", "2", false},
        {"width", "200px", true},
        {"display", "none", true},
        {"font-size", "20px", true},
        {"white-space", "break-spaces", true},
        {"field-sizing", "fixed", true},
        {"border-top-style", "none", true},
        {"direction", "ltr", true},
        {"margin-left", "9px", true},
        {"row-gap", "7px", true},
    };
    for (const LonghandProbe& longhand : kLonghands) {
        const bool changed = signature_after([&longhand](LayoutInput& n) {
                                 n.style.set(longhand.property, longhand.value);
                             }) != probe_signature;
        if (changed != longhand.included) {
            std::cerr << "layout_signature: " << longhand.property
                      << (longhand.included ? " is missing from the signature\n"
                                            : " must be excluded from the signature\n");
            ++failures;
        }
    }

    check(signature_after([](LayoutInput& n) { n.text = "y"; }) != probe_signature,
          "text is included");
    check(signature_after([](LayoutInput& n) { n.id = 43; }) != probe_signature, "id is included");
    check(signature_after([](LayoutInput& n) { n.children.push_back(LayoutInput{}); }) !=
              probe_signature,
          "child count is included");

    const LayoutViewport wide{probe_viewport.width + 1.0f, probe_viewport.height,
                              probe_viewport.dpi_scale};
    const LayoutViewport tall{probe_viewport.width, probe_viewport.height + 1.0f,
                              probe_viewport.dpi_scale};
    const LayoutViewport dense{probe_viewport.width, probe_viewport.height, 2.0f};
    check(tessera::ui::layout::layout_signature(probe, wide) != probe_signature &&
              tessera::ui::layout::layout_signature(probe, tall) != probe_signature &&
              tessera::ui::layout::layout_signature(probe, dense) != probe_signature,
          "viewport width/height/dpi scale are included");

    // `ComputedStyle` is a sorted map, so declaration insertion order cannot fake a change.
    LayoutInput order_a = node(9, {{"width", "10px"}, {"height", "5px"}});
    LayoutInput order_b = node(9, {{"height", "5px"}, {"width", "10px"}});
    check(tessera::ui::layout::layout_signature(order_a, probe_viewport) ==
              tessera::ui::layout::layout_signature(order_b, probe_viewport),
          "signature ignores declaration insertion order");

    if (failures != 0) {
        std::cerr << "layout_signature: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "layout_signature: ok\n";
    return 0;
}
