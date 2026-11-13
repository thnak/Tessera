// SPDX-License-Identifier: MIT
//
// Scroll style properties wired to the scroll controller (issue #248, docs/specs/stylesheet-v1.md
// section 11, docs/specs/scroll-and-gestures-v1.md sections 2 and 4): the registry grammar and
// diagnostics, `@supports`, the computed-style -> scene -> controller path (`scroll-behavior`,
// `overscroll-behavior`/`allow_chain`, `overscroll-effect`), smooth versus jump programmatic scroll
// with the reduced-motion clamp, and `scrollbar-gutter: stable` as layout input (reserved width and
// the layout-domain signature). Deterministic: fixed stylesheets, fixed dt, no clock or GPU.

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_signature.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/scroll.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/scene_style.hpp"
#include "tessera/ui/style/scroll_style_sync.hpp"
#include "tessera/ui/style/supports.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::advance_scroll_animation;
using tessera::ui::allow_chain;
using tessera::ui::animates_overscroll;
using tessera::ui::apply_wheel_scroll;
using tessera::ui::Dirty;
using tessera::ui::has_flag;
using tessera::ui::Node;
using tessera::ui::OverscrollBehavior;
using tessera::ui::OverscrollEffect;
using tessera::ui::scroll_by_programmatic;
using tessera::ui::scroll_to_programmatic;
using tessera::ui::ScrollBehavior;
using tessera::ui::layout::layout_signature;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::Declaration;
using tessera::ui::style::evaluate_supports;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::SceneStyleTree;
using tessera::ui::style::StyleDiagnosticKind;
using tessera::ui::style::sync_scroll_style;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scroll_properties: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

bool supports(std::string_view text, const PropertyRegistry& registry) {
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, registry);
}

const LayoutBox* find_box(const LayoutResult& result, std::uint64_t id) {
    for (const auto& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

bool registry_entries() {
    PropertyRegistry registry;
    const auto* behavior = registry.find("scroll-behavior");
    const auto* overscroll = registry.find("overscroll-behavior");
    const auto* effect = registry.find("overscroll-effect");
    const auto* gutter = registry.find("scrollbar-gutter");
    if (!expect(behavior != nullptr && !behavior->inherited && behavior->initial == "auto",
                "scroll-behavior is a non-inherited auto property") ||
        !expect(overscroll != nullptr && !overscroll->inherited && overscroll->initial == "auto",
                "overscroll-behavior is a non-inherited auto property") ||
        !expect(effect != nullptr && !effect->inherited && effect->initial == "platform",
                "overscroll-effect initial is the animation-v1 Platform request") ||
        !expect(gutter != nullptr && !gutter->inherited && gutter->initial == "auto",
                "scrollbar-gutter is a non-inherited auto property")) {
        return false;
    }

    // Every accepted keyword reaches the declaration; the two-value forms and unknown keywords are
    // dropped with exactly one UnsupportedValue diagnostic (stylesheet-v1 section 11).
    const struct {
        const char* property;
        std::vector<const char*> good;
        std::vector<const char*> bad;
    } cases[] = {
        {"scroll-behavior", {"auto", "smooth"}, {"linear", "auto smooth"}},
        {"overscroll-behavior", {"auto", "contain", "none"}, {"scroll", "contain none"}},
        {"overscroll-effect",
         {"none", "glow", "stretch", "bounce", "platform"},
         {"auto", "glow stretch"}},
        {"scrollbar-gutter", {"auto", "stable"}, {"stable both-edges", "scroll"}},
    };
    for (const auto& entry : cases) {
        for (const char* value : entry.good) {
            const auto expanded = registry.expand(Declaration{entry.property, value, false, 7});
            if (!expect(expanded.diagnostics.empty() && expanded.declarations.size() == 1,
                        "accepted scroll value produces one declaration") ||
                !expect(expanded.declarations[0].offset == 7, "diagnostic/declaration offset")) {
                return false;
            }
        }
        for (const char* value : entry.bad) {
            const auto expanded = registry.expand(Declaration{entry.property, value, false, 9});
            if (!expect(expanded.declarations.empty(), "bad scroll value drops the declaration") ||
                !expect(expanded.diagnostics.size() == 1 &&
                            expanded.diagnostics[0].kind == StyleDiagnosticKind::UnsupportedValue,
                        "bad scroll value reports UnsupportedValue") ||
                !expect(expanded.diagnostics[0].offset == 9, "bad scroll value offset")) {
                return false;
            }
        }
        // A custom-property reference is substituted later, so it must survive validation.
        const auto variable = registry.expand(Declaration{entry.property, "var(--x)", false, 0});
        if (!expect(variable.diagnostics.empty() && variable.declarations.size() == 1,
                    "var() reference reaches computed-value substitution")) {
            return false;
        }
    }
    return true;
}

bool supports_grammar() {
    const PropertyRegistry registry;
    return expect(supports("(overscroll-behavior: contain)", registry),
                  "@supports accepts overscroll-behavior: contain") &&
           expect(supports("(scroll-behavior: smooth)", registry),
                  "@supports accepts scroll-behavior: smooth") &&
           expect(supports("(overscroll-effect: stretch)", registry),
                  "@supports accepts overscroll-effect: stretch") &&
           expect(supports("(scrollbar-gutter: stable)", registry),
                  "@supports accepts scrollbar-gutter: stable") &&
           expect(!supports("(overscroll-behavior: chain)", registry),
                  "@supports rejects an unknown overscroll-behavior keyword") &&
           expect(!supports("(overscroll-effect: rubber-band)", registry),
                  "@supports rejects an unknown overscroll-effect keyword");
}

void configure_container(Node& node, float height) {
    node.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    node.set_scrollable_y(true);
    node.set_content_size(100.0f, height);
}

bool style_reaches_controller() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    auto smooth_ptr = std::make_unique<Node>(2);
    smooth_ptr->set_class_name("smooth contain");
    Node& smooth = root.add_child(std::move(smooth_ptr));
    auto chain_ptr = std::make_unique<Node>(3);
    chain_ptr->set_class_name("chain");
    Node& chain = root.add_child(std::move(chain_ptr));

    const auto compiled =
        compile_stylesheet(".smooth { scroll-behavior: smooth; overscroll-behavior: contain; "
                           "overscroll-effect: bounce; }\n"
                           ".chain { overscroll-behavior: auto; overscroll-effect: stretch; }\n");
    SceneStyleTree tree(root);
    root.clear_dirty(Dirty::All);
    smooth.clear_dirty(Dirty::All);
    chain.clear_dirty(Dirty::All);
    const auto result = sync_scroll_style(root, tree, compiled);
    if (!expect(result.nodes == 3, "sync visits every node") ||
        !expect(result.changed == 2, "sync changes the two styled nodes")) {
        return false;
    }
    if (!expect(smooth.scroll_behavior() == ScrollBehavior::Smooth,
                "scroll-behavior: smooth reaches the node") ||
        !expect(smooth.overscroll_behavior() == OverscrollBehavior::Contain,
                "overscroll-behavior: contain reaches the node") ||
        !expect(smooth.overscroll_effect() == OverscrollEffect::Bounce,
                "overscroll-effect: bounce reaches the node") ||
        !expect(!allow_chain(smooth), "contain does not chain") ||
        !expect(!animates_overscroll(smooth), "contain does not animate past the edge")) {
        return false;
    }
    if (!expect(chain.overscroll_behavior() == OverscrollBehavior::Auto,
                "default overscroll-behavior is auto") ||
        !expect(chain.overscroll_effect() == OverscrollEffect::Stretch,
                "overscroll-effect: stretch reaches the node") ||
        !expect(allow_chain(chain), "auto chains") ||
        !expect(animates_overscroll(chain), "auto with a non-none effect animates past the edge")) {
        return false;
    }

    // A steady scene stays clean: a second sync changes nothing.
    SceneStyleTree second_tree(root);
    const auto steady = sync_scroll_style(root, second_tree, compiled);
    if (!expect(steady.changed == 0, "a steady scene reports no scroll style change")) {
        return false;
    }

    // The controller itself never dirties layout from these runtime inputs.
    if (!expect(!has_flag(smooth.dirty(), Dirty::Layout) && !has_flag(chain.dirty(), Dirty::Layout),
                "scroll runtime inputs never mark Layout")) {
        return false;
    }
    return true;
}

bool smooth_versus_jump() {
    Node container(10);
    configure_container(container, 300.0f);
    container.set_scroll_behavior(ScrollBehavior::Smooth);

    // A smooth request stores a clamped target and moves nothing yet.
    if (!expect(scroll_to_programmatic(container, 0.0f, 120.0f), "smooth request is accepted") ||
        !expect(near(container.scroll_y(), 0.0f), "smooth request does not jump") ||
        !expect(container.scroll_animation_active(), "smooth request starts an animation")) {
        return false;
    }
    // Targets are clamped to the scroll range (200 px) at request time.
    (void)scroll_to_programmatic(container, 0.0f, 1000.0f);
    if (!expect(near(container.scroll_animation_target_y(), 200.0f),
                "smem target clamps to the maximum offset")) {
        return false;
    }

    float previous = container.scroll_y();
    bool monotonic = true;
    std::size_t ticks = 0;
    while (container.scroll_animation_active() && ticks < 1000) {
        (void)advance_scroll_animation(container, 1.0 / 60.0);
        monotonic = monotonic && container.scroll_y() >= previous && container.scroll_y() <= 200.0f;
        previous = container.scroll_y();
        ++ticks;
    }
    if (!expect(monotonic, "smooth scroll advances monotonically inside the range") ||
        !expect(!container.scroll_animation_active(), "smooth scroll settles") ||
        !expect(near(container.scroll_y(), 200.0f), "smooth scroll lands exactly on the target") ||
        !expect(ticks > 1, "smooth scroll needs more than one fixed step")) {
        return false;
    }

    // A zero dt tick must not move, and a retarget mid-flight must not jump.
    (void)scroll_to_programmatic(container, 0.0f, 0.0f);
    (void)advance_scroll_animation(container, 1.0 / 60.0);
    const float before_zero = container.scroll_y();
    (void)advance_scroll_animation(container, 0.0);
    if (!expect(near(container.scroll_y(), before_zero), "zero dt does not move") ||
        !expect(scroll_to_programmatic(container, 0.0f, 50.0f), "retarget accepted") ||
        !expect(container.scroll_y() == before_zero, "retarget does not jump")) {
        return false;
    }
    for (int i = 0; i < 1000 && container.scroll_animation_active(); ++i) {
        (void)advance_scroll_animation(container, 1.0 / 60.0);
    }
    if (!expect(near(container.scroll_y(), 50.0f), "retargeted scroll lands on the new target")) {
        return false;
    }

    // Reduced motion clamps smooth to a jump, for both the request and an in-flight animation.
    container.set_scroll_behavior(ScrollBehavior::Auto);
    if (!expect(scroll_to_programmatic(container, 0.0f, 10.0f, true),
                "auto request still applies") ||
        !expect(near(container.scroll_y(), 10.0f), "auto request jumps immediately") ||
        !expect(!container.scroll_animation_active(), "auto request leaves no animation")) {
        return false;
    }
    container.set_scroll_behavior(ScrollBehavior::Smooth);
    (void)scroll_to_programmatic(container, 0.0f, 150.0f);
    if (!expect(container.scroll_animation_active(),
                "smooth request active before reduced motion") ||
        !expect(!advance_scroll_animation(container, 1.0 / 60.0, true),
                "reduced motion ends the animation") ||
        !expect(near(container.scroll_y(), 150.0f), "reduced motion jumps to the target")) {
        return false;
    }
    // scroll_by_programmatic honours the same policy.
    if (!expect(scroll_by_programmatic(container, 0.0f, -20.0f, true),
                "programmatic by applies under reduced motion") ||
        !expect(near(container.scroll_y(), 130.0f), "programmatic by jumps by the delta")) {
        return false;
    }
    return true;
}

bool chaining_and_overscroll() {
    Node root(20);
    root.set_bounds({0, 0, 300, 300});
    auto outer_ptr = std::make_unique<Node>(21);
    outer_ptr->set_bounds({0, 0, 200, 200});
    outer_ptr->set_scrollable_y(true);
    outer_ptr->set_content_size(200.0f, 500.0f); // max 300
    Node& outer = root.add_child(std::move(outer_ptr));
    auto inner_ptr = std::make_unique<Node>(22);
    inner_ptr->set_bounds({0, 0, 100, 100});
    inner_ptr->set_scrollable_y(true);
    inner_ptr->set_content_size(100.0f, 300.0f); // max 200
    Node& inner = outer.add_child(std::move(inner_ptr));
    auto leaf_ptr = std::make_unique<Node>(23);
    leaf_ptr->set_bounds({0, 0, 20, 20});
    Node& leaf = inner.add_child(std::move(leaf_ptr));

    // The inner container is at its top edge; an upward wheel chains to the outer auto container.
    outer.set_scroll_offset(0.0f, 100.0f);
    if (!expect(apply_wheel_scroll(leaf, 0.0f, -1.0f), "chained wheel changes an offset") ||
        !expect(near(inner.scroll_y(), 0.0f), "inner stays clamped at its edge") ||
        !expect(near(outer.scroll_y(), 60.0f),
                "auto chains the remainder to the outer container")) {
        return false;
    }

    // `contain` and `none` drop the remainder at their edge.
    for (const OverscrollBehavior behavior :
         {OverscrollBehavior::Contain, OverscrollBehavior::None}) {
        inner.set_overscroll_behavior(behavior);
        outer.set_scroll_offset(0.0f, 100.0f);
        if (!expect(!apply_wheel_scroll(leaf, 0.0f, -1.0f),
                    "edge wheel changes nothing when chaining is off") ||
            !expect(near(outer.scroll_y(), 100.0f), "contain/none never chain the remainder") ||
            !expect(!allow_chain(inner), "contain/none allow_chain is false")) {
            return false;
        }
    }

    // A partially scrolled inner container consumes the delta itself.
    inner.set_overscroll_behavior(OverscrollBehavior::Auto);
    (void)inner.set_scroll_offset(0.0f, 100.0f);
    if (!expect(apply_wheel_scroll(leaf, 0.0f, -1.0f), "inner consumes a partial wheel") ||
        !expect(near(inner.scroll_y(), 60.0f), "inner consumes 40 px") ||
        !expect(near(outer.scroll_y(), 100.0f), "outer is untouched while inner can consume")) {
        return false;
    }

    // `overscroll-effect` policy: contain/none suppress the visual edge animation, `none` too.
    inner.set_overscroll_effect(OverscrollEffect::Stretch);
    if (!expect(animates_overscroll(inner), "auto + stretch animates past the edge")) {
        return false;
    }
    inner.set_overscroll_behavior(OverscrollBehavior::Contain);
    if (!expect(!animates_overscroll(inner), "contain suppresses the edge animation")) {
        return false;
    }
    inner.set_overscroll_behavior(OverscrollBehavior::Auto);
    inner.set_overscroll_effect(OverscrollEffect::None);
    if (!expect(!animates_overscroll(inner),
                "overscroll-effect: none suppresses the edge animation")) {
        return false;
    }
    return true;
}

LayoutInput gutter_scene(const char* gutter, const char* direction) {
    LayoutInput root;
    root.id = 1;
    root.style.set("display", "flex");
    root.style.set("flex-direction", "column");
    root.style.set("width", "200px");
    root.style.set("height", "100px");
    root.style.set("box-sizing", "border-box");
    root.style.set("overflow-y", "auto");
    root.style.set("scrollbar-gutter", gutter);
    root.style.set("direction", direction);

    LayoutInput child;
    child.id = 2;
    child.style.set("height", "50px");
    root.children.push_back(std::move(child));
    return root;
}

bool gutter_is_layout_input() {
    LayoutEngine engine;
    LayoutViewport viewport;
    viewport.width = 320.0f;
    viewport.height = 200.0f;

    const auto stable = engine.layout(gutter_scene("stable", "ltr"), viewport);
    const auto automatic = engine.layout(gutter_scene("auto", "ltr"), viewport);
    const auto* stable_child = find_box(stable, 2);
    const auto* automatic_child = find_box(automatic, 2);
    if (!expect(stable_child != nullptr && automatic_child != nullptr, "gutter scene lays out") ||
        !expect(near(automatic_child->bounds.width, 200.0f), "auto does not reserve a gutter") ||
        !expect(near(stable_child->bounds.width, 192.0f),
                "stable reserves the 8 px overlay scrollbar width") ||
        !expect(near(stable_child->bounds.x, 0.0f), "LTR gutter is on the inline end")) {
        return false;
    }
    const auto* stable_root = find_box(stable, 1);
    if (!expect(stable_root != nullptr && near(stable_root->content_insets.right, 8.0f),
                "stable is part of the content insets")) {
        return false;
    }

    // RTL mirrors the reservation to the inline start (physical left) side.
    const auto rtl = engine.layout(gutter_scene("stable", "rtl"), viewport);
    const auto* rtl_child = find_box(rtl, 2);
    const auto* rtl_root = find_box(rtl, 1);
    if (!expect(rtl_child != nullptr && rtl_root != nullptr, "RTL gutter scene lays out") ||
        !expect(near(rtl_root->content_insets.left, 8.0f), "RTL gutter is on the physical left") ||
        !expect(near(rtl_child->bounds.x, 8.0f) && near(rtl_child->bounds.width, 192.0f),
                "RTL children are inset by the gutter")) {
        return false;
    }

    // A non-scroll container never reserves the gutter.
    LayoutInput plain = gutter_scene("stable", "ltr");
    plain.style.erase("overflow-y");
    const auto plain_result = engine.layout(plain, viewport);
    const auto* plain_child = find_box(plain_result, 2);
    if (!expect(plain_child != nullptr && near(plain_child->bounds.width, 200.0f),
                "scrollbar-gutter requires a scroll container")) {
        return false;
    }

    // Dirty domain: `scrollbar-gutter` is a layout input, the other three are not.
    const auto signature_of = [&viewport](const char* property, const char* value) {
        LayoutInput input = gutter_scene("auto", "ltr");
        input.style.set(property, value);
        return tessera::ui::layout::layout_signature(input, viewport);
    };
    const auto base = signature_of("scroll-behavior", "auto");
    if (!expect(signature_of("scrollbar-gutter", "stable") != base,
                "scrollbar-gutter changes the layout signature") ||
        !expect(signature_of("scroll-behavior", "smooth") == base,
                "scroll-behavior never changes the layout signature") ||
        !expect(signature_of("overscroll-behavior", "contain") == base,
                "overscroll-behavior never changes the layout signature") ||
        !expect(signature_of("overscroll-effect", "stretch") == base,
                "overscroll-effect never changes the layout signature")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&failures](bool condition) {
        if (!condition) {
            ++failures;
        }
    };
    check(registry_entries());
    check(supports_grammar());
    check(style_reaches_controller());
    check(smooth_versus_jump());
    check(chaining_and_overscroll());
    check(gutter_is_layout_input());
    if (failures != 0) {
        return 1;
    }
    std::cout << "scroll_properties: ok\n";
    return 0;
}
