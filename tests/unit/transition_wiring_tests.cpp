// SPDX-License-Identifier: MIT
//
// Transition and animation declarations reach the frame pipeline (issue #271). Before this, a
// stylesheet could declare `transition: opacity 200ms linear` or `animation: fade 1s` and nothing
// ran: `start_style_transitions` and `sync_style_animations` had test callers only, so those names
// were registered (and `@supports`-visible) but had no consumer.
//
// These tests drive the whole path: a real `CompiledStyleSheet` over a real `SceneStyleTree`, the
// scene mutated so the computed style changes, `FramePipeline::sync_style` (the style stage a frame
// runs), then a tick of the pipeline and the sampled overlay applied to a `ComputedStyle`. A
// parse-only assertion would pass without any of that, which is exactly the gap being closed, so
// every check here observes an `AnimationId` from the pipeline and the value it produces.
//
// Deterministic: a manual clock, no GPU, no wall clock.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/animation_style.hpp"
#include "tessera/ui/animation/style_overlay.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::apply_animation_result;
using tessera::ui::animation::parse_style_transitions;
using tessera::ui::animation::StyleBridgeResult;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::SceneStyleProvider;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::CompiledStyleSheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::SceneStyleTree;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "transition_wiring: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs, float tolerance = 0.01f) {
    return std::abs(lhs - rhs) <= tolerance;
}

float number(const ComputedStyle& style, const char* property, bool& found) {
    const std::string* value = style.find(property);
    found = value != nullptr;
    return found ? std::stof(*value) : -1.0f;
}

// A frame pipeline over a real cascade: a compiled stylesheet, a selector tree captured from the
// scene, and the provider that borrows both, exactly as AppHost assembles them. `refresh()` mirrors
// AppHost::Impl::rebuild_style_tree after a structural or class change.
class Fixture {
public:
    explicit Fixture(const CompiledStyleSheet& sheet)
        : clock_(),
          loop_(clock_),
          damage_(DamageConfig{Extent2D{400, 400}, 8}),
          pipeline_(animations_, loop_, tracker_, damage_, dirty_),
          sheet_(&sheet) {
        root_.set_bounds({0.0f, 0.0f, 400.0f, 400.0f});
        refresh();
        clear_dirty();
    }

    void refresh() {
        tree_ = std::make_unique<SceneStyleTree>(root_);
        provider_ = std::make_unique<SceneStyleProvider>(*sheet_, *tree_);
    }

    // One frame: tick the pipeline, run the style stage, finish.
    FrameTick frame(double dt) {
        FrameTick tick = pipeline_.tick(root_, dt);
        pipeline_.sync_style(root_, *provider_, tick, true);
        (void)pipeline_.finish_frame(root_);
        return tick;
    }

    // The `box` child every test drives: id 2, one class list, one 40x40 box.
    Node& add_box(const char* classes) {
        Node& box = root_.add_child(std::make_unique<Node>(2));
        box.set_class_name(classes);
        box.set_bounds({0.0f, 0.0f, 40.0f, 40.0f});
        refresh();
        clear_dirty();
        return box;
    }

    // A class change is a style change: mark it and rebuild the selector tree, as the host does.
    void set_classes(Node& node, const char* classes) {
        node.set_class_name(classes);
        node.mark_dirty(Dirty::Style);
        refresh();
    }

    void clear_dirty() {
        root_.clear_dirty(Dirty::All);
        for (const auto& child : root_.children()) {
            child->clear_dirty(Dirty::All);
        }
    }

    [[nodiscard]] Node& root() {
        return root_;
    }
    [[nodiscard]] FramePipeline& pipeline() {
        return pipeline_;
    }
    [[nodiscard]] AnimationRuntime& animations() {
        return animations_;
    }
    [[nodiscard]] ComputedStyle compute(const Node& node) {
        return provider_->compute(node);
    }

private:
    ManualClock clock_;
    RuntimeLoop loop_;
    DamageTracker tracker_;
    DamageAccumulator damage_;
    DirtyPass dirty_;
    Node root_{1};
    AnimationRuntime animations_;
    FramePipeline pipeline_;
    const CompiledStyleSheet* sheet_ = nullptr;
    std::unique_ptr<SceneStyleTree> tree_;
    std::unique_ptr<SceneStyleProvider> provider_;
};

} // namespace

int main() {
    // --- parse_style_transitions: longhands win, shorthand is the fallback ----------------------
    {
        ComputedStyle shorthand;
        shorthand.set("transition", "opacity 200ms linear");
        const auto parsed = parse_style_transitions(shorthand);
        if (!expect(parsed.ok && parsed.transitions.size() == 1 &&
                        parsed.transitions[0].property == "opacity" &&
                        near(parsed.transitions[0].duration, 0.2f),
                    "shorthand parses")) {
            return 1;
        }

        ComputedStyle longhands;
        longhands.set("transition", "opacity 9s linear");
        longhands.set("transition-property", "width");
        longhands.set("transition-duration", "150ms");
        longhands.set("transition-timing-function", "ease-in");
        longhands.set("transition-delay", "20ms");
        const auto prefer = parse_style_transitions(longhands);
        if (!expect(prefer.ok && prefer.transitions.size() == 1 &&
                        prefer.transitions[0].property == "width" &&
                        near(prefer.transitions[0].duration, 0.15f) &&
                        near(prefer.transitions[0].delay, 0.02f),
                    "longhands win over the shorthand")) {
            return 2;
        }

        const auto none = parse_style_transitions(ComputedStyle{});
        if (!expect(none.ok && none.transitions.empty(), "undeclared transition parses empty")) {
            return 3;
        }
    }

    // --- end to end: a stylesheet transition starts and its overlay moves the sampled style -----
    {
        const CompiledStyleSheet sheet =
            compile_stylesheet(".box { opacity: 1; }"
                               ".box.dim { opacity: 0.25; }"
                               ".animating { transition: opacity 200ms linear; }");
        if (!expect(sheet.diagnostics().empty(), "transition stylesheet compiles")) {
            return 4;
        }

        Fixture fixture(sheet);
        Node& box = fixture.add_box("box animating");

        // The first style stage only records the baseline: the initial style never transitions.
        fixture.frame(1.0 / 60.0);
        const StyleBridgeResult baseline = fixture.pipeline().style_bridge();
        if (!expect(baseline.nodes == 2, "the walk covers root and box") ||
            !expect(baseline.transitions_started == 0, "the initial style starts no transition") ||
            !expect(fixture.animations().active() == 0, "nothing is running yet")) {
            return 5;
        }

        // Change the computed `opacity`. The new style declares the transition, so the diff starts
        // one: this is the whole point of the issue, and before the bridge it stayed at zero.
        fixture.set_classes(box, "box dim animating");
        fixture.frame(1.0 / 60.0);
        const StyleBridgeResult started = fixture.pipeline().style_bridge();
        if (!expect(started.transitions_started == 1, "the opacity change starts one transition") ||
            !expect(started.ids.size() == 1, "it reports one AnimationId") ||
            !expect(started.ids[0] != 0, "the id is non-zero") ||
            !expect(fixture.animations().active() == 1, "the runtime has the transition")) {
            return 6;
        }
        const auto& entry = fixture.animations().entries().front();
        if (!expect(entry.target == 2, "the transition targets the box") ||
            !expect(entry.property == "opacity", "the transition tracks opacity") ||
            !expect(entry.kind == tessera::ui::animation::AnimationKind::Transition,
                    "it is a transition, not a keyframe animation")) {
            return 7;
        }

        // Tick halfway through and apply the sampled overlay: this is what layout and paint resolve
        // against, so the value has to move, not just the bookkeeping.
        const FrameTick midway = fixture.pipeline().tick(fixture.root(), 0.1);
        if (!expect(!midway.animation.samples.empty(), "the tick samples the transition")) {
            return 8;
        }
        ComputedStyle animated = fixture.compute(box);
        apply_animation_result(animated, midway.animation, 2);
        bool found = false;
        const float opacity = number(animated, "opacity", found);
        // 0.1 s of a 200 ms linear transition from 1 to 0.25 is halfway: 0.625.
        if (!expect(found && near(opacity, 0.625f), "the overlay moves opacity halfway")) {
            return 9;
        }
        if (!expect(midway.animation.keep_awake, "the running transition keeps the loop awake")) {
            return 10;
        }

        // Finishing the transition settles at the new value and frees the loop.
        const FrameTick end = fixture.pipeline().tick(fixture.root(), 0.2);
        if (!expect(!end.animation.finished.empty(), "the transition finishes") ||
            !expect(fixture.animations().empty(), "the runtime settles")) {
            return 11;
        }
    }

    // --- a property the stylesheet does not transition never starts one -------------------------
    {
        const CompiledStyleSheet sheet = compile_stylesheet(".box { opacity: 1; }"
                                                            ".box.dim { opacity: 0.25; }");
        Fixture fixture(sheet);
        Node& box = fixture.add_box("box");
        fixture.frame(1.0 / 60.0);

        fixture.set_classes(box, "box dim");
        fixture.frame(1.0 / 60.0);
        if (!expect(fixture.pipeline().style_bridge().transitions_started == 0,
                    "a change with no transition declared starts nothing") ||
            !expect(fixture.animations().empty(), "the runtime stays empty")) {
            return 12;
        }
    }

    // --- zero duration: the value applies, no entry is left behind ------------------------------
    {
        const CompiledStyleSheet sheet =
            compile_stylesheet(".box { opacity: 1; }"
                               ".box.dim { opacity: 0.25; }"
                               ".t { transition: opacity 0s linear; }");
        Fixture fixture(sheet);
        Node& box = fixture.add_box("box t");
        fixture.frame(1.0 / 60.0);

        fixture.set_classes(box, "box dim t");
        fixture.frame(1.0 / 60.0);
        if (!expect(fixture.pipeline().style_bridge().transitions_started == 0,
                    "a zero-duration transition starts no entry") ||
            !expect(fixture.animations().empty(), "nothing is left running")) {
            return 13;
        }
    }

    // --- an interrupted transition continues from the displayed value ---------------------------
    {
        const CompiledStyleSheet sheet =
            compile_stylesheet(".box { opacity: 1; }"
                               ".box.half { opacity: 0.5; }"
                               ".box.gone { opacity: 0.0; }"
                               ".t { transition: opacity 200ms linear; }");
        Fixture fixture(sheet);
        Node& box = fixture.add_box("box t");
        fixture.frame(1.0 / 60.0);

        fixture.set_classes(box, "box half t");
        fixture.frame(1.0 / 60.0);
        if (!expect(fixture.animations().active() == 1, "the first transition runs")) {
            return 14;
        }

        // Advance to the halfway sample, then retarget to 0. The new transition must continue from
        // the displayed ~0.75, not snap back to the 1 the first transition started from.
        (void)fixture.pipeline().tick(fixture.root(), 0.06);
        fixture.set_classes(box, "box gone t");
        fixture.frame(1.0 / 60.0);
        if (!expect(fixture.pipeline().style_bridge().transitions_started == 1,
                    "the retarget starts a new transition")) {
            return 15;
        }
        const FrameTick resumed = fixture.pipeline().tick(fixture.root(), 0.0);
        ComputedStyle shown = fixture.compute(box);
        apply_animation_result(shown, resumed.animation, 2);
        bool found = false;
        const float opacity = number(shown, "opacity", found);
        // At the retarget instant the new entry shows its own "from" value, which must be the
        // interpolated mid-flight value rather than the original 1.0.
        if (!expect(found && opacity < 0.95f && opacity > 0.5f,
                    "the interrupted transition continues from the displayed value")) {
            return 16;
        }
    }

    // --- the animation half of the bridge drives `animation-name` -------------------------------
    {
        const CompiledStyleSheet sheet =
            compile_stylesheet("@keyframes fade { from { opacity: 1 } to { opacity: 0 } }"
                               ".box { animation: fade 400ms linear; }");
        Fixture fixture(sheet);
        fixture.pipeline().set_style_keyframes(&sheet.keyframes());
        Node& box = fixture.add_box("box");
        fixture.frame(1.0 / 60.0);

        const StyleBridgeResult started = fixture.pipeline().style_bridge();
        if (!expect(started.animations_started == 1, "the keyframe animation starts") ||
            !expect(fixture.animations().active() == 1, "the runtime has the animation")) {
            return 17;
        }

        const FrameTick midway = fixture.pipeline().tick(fixture.root(), 0.2);
        ComputedStyle animated = fixture.compute(box);
        apply_animation_result(animated, midway.animation, 2);
        bool found = false;
        const float opacity = number(animated, "opacity", found);
        if (!expect(found && near(opacity, 0.5f), "the animation overlay moves opacity halfway")) {
            return 18;
        }

        // Removing the declaration stops it on the next style stage.
        fixture.set_classes(box, "");
        fixture.frame(1.0 / 60.0);
        if (!expect(fixture.pipeline().style_bridge().animations_stopped >= 1,
                    "removing animation-name stops it") ||
            !expect(fixture.animations().active() == 0, "the runtime stops the animation")) {
            return 19;
        }
    }

    // --- a `visibility: hidden` node leaves the walk and drops its snapshot ---------------------
    {
        const CompiledStyleSheet sheet =
            compile_stylesheet(".box { opacity: 1; }"
                               ".box.dim { opacity: 0.25; }"
                               ".t { transition: opacity 200ms linear; }"
                               ".hide { visibility: hidden; }");
        Fixture fixture(sheet);
        Node& box = fixture.add_box("box t");
        fixture.frame(1.0 / 60.0);
        if (!expect(fixture.pipeline().style_bridge().nodes == 2, "the box is walked")) {
            return 20;
        }

        fixture.set_classes(box, "box dim t hide");
        fixture.frame(1.0 / 60.0);
        if (!expect(fixture.pipeline().style_bridge().nodes == 1,
                    "a hidden node leaves the walk") ||
            !expect(fixture.animations().empty(), "hiding it starts no transition")) {
            return 21;
        }
    }

    // --- a node with no transition declaration is not tracked -----------------------------------
    {
        const CompiledStyleSheet sheet = compile_stylesheet(".box { opacity: 1 }");
        Fixture fixture(sheet);
        (void)fixture.add_box("box");
        fixture.frame(1.0 / 60.0);
        if (!expect(fixture.pipeline().style_bridge().nodes == 2, "both nodes are walked") ||
            !expect(fixture.pipeline().style_bridge().transitions_started == 0, "nothing starts") ||
            !expect(fixture.pipeline().style_bridge().ids.empty(), "no ids are reported")) {
            return 22;
        }
    }

    std::cout << "transition_wiring: ok\n";
    return 0;
}
