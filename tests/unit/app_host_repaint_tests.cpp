// SPDX-License-Identifier: MIT
//
// A host that has never painted must plan a full-target frame (the Android fold defect).
//
// **The defect this file pins.** `FramePipeline::tick` clears the damage accumulator and derives
// the redraw plan, and `should_render`, from the dirty nodes it collects. A host therefore paints
// only when something is dirty. On a freshly launched app that is true once, because the first
// layout dirties every node - which hid the bug. On Android the surface is destroyed whenever the
// device folds shut or the app backgrounds, and the host is rebuilt over the shell's LIVE scene:
// those nodes are already laid out and clean, so the new host's first tick planned nothing, the
// loop parked, and the window stayed black until a touch dirtied a node.
//
// **The rule.** Host construction latches a full repaint. A host has painted no pixels, so whatever
// the previous host left on the surface (or whatever the platform discarded) cannot describe the
// screen, and a damage-only first frame would be wrong even if it planned something.
//
// This is the headless half of the emulator check (docs/guides/android-lane.md section 3.19): the
// host-shaped reproduction is deterministic and needs no device.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_host_repaint: " << message << "\n";
    }
    return condition;
}

// Counts what reached the sink, so "painted" is a fact about commands rather than a flag.
class CountingSink final : public tessera::ui::paint::PaintSink {
public:
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {
        ++fills;
    }
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {
        ++fills;
    }
    void draw_text(const tessera::ui::paint::TextDraw&) override {
        ++texts;
    }

    int fills = 0;
    int texts = 0;
};

PaintData text_payload(std::string text) {
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::move(text);
    return data;
}

// One scene, built once, so two hosts can be pointed at the same tree exactly as the Android shell
// does across a surface's destroy/create pair.
void build(Node& root) {
    root.set_element_kind("View");
    root.set_inline_style("width", "100%");
    root.set_inline_style("height", "100%");
    root.set_inline_style("background-color", "#0b1220");

    auto label = std::make_unique<Node>(2);
    label->set_element_kind("Text");
    label->set_paint_data(text_payload("Taps: 0"));
    root.add_child(std::move(label));
}

tessera::app::HostConfig config_for() {
    tessera::app::HostConfig config;
    config.width = 320.0f;
    config.height = 200.0f;
    return config;
}

} // namespace

int main() {
    // --- 1. The control: a fresh scene is dirty, so the first frame paints anyway ------------
    // This is the path that made the bug invisible at app launch. It must keep working: the fix is
    // additive, not a replacement for the dirty-driven path.
    {
        Node root(1);
        build(root);
        auto sheet = compile_stylesheet(R"(
View { background-color: #0b1220; }
Text { font-size: 16px; color: #e2e8f0; }
)");
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        tessera::app::AppHost host(root, std::move(sheet), loop, config_for());

        const tessera::app::HostFrame frame = host.begin_frame(0.0);
        CountingSink sink;
        const auto paint = host.paint(sink);
        if (!expect(frame.frame_due, "a fresh scene makes the first frame due") ||
            !expect(frame.render.should_render, "a fresh scene plans a first frame") ||
            !expect(paint.painted, "a fresh scene's first frame paints") ||
            !expect(sink.fills > 0, "a fresh scene's first frame emits fills")) {
            return 1;
        }
        host.end_frame();
    }

    // --- 2. The defect: a second host over the SAME settled tree, with no change between ------
    // This is the fold cycle. The tree is clean; nothing was edited, resized or focused. A host
    // that relied on dirty flags alone would plan an empty frame here, which is the black window.
    {
        Node root(1);
        build(root);
        auto sheet = compile_stylesheet(R"(
View { background-color: #0b1220; }
Text { font-size: 16px; color: #e2e8f0; }
)");

        // First host: physics the tree into its settled state, then goes away (surface destroyed).
        {
            tessera::runtime::ManualClock clock;
            tessera::runtime::RuntimeLoop loop(clock);
            tessera::app::AppHost host(root, std::move(sheet), loop, config_for());

            const tessera::app::HostFrame frame = host.begin_frame(0.0);
            CountingSink sink;
            (void)host.paint(sink);
            host.end_frame();
            if (!expect(frame.frame_due, "the setup host painted its first frame")) {
                return 2;
            }
            sheet = compile_stylesheet(R"(
View { background-color: #0b1220; }
Text { font-size: 16px; color: #e2e8f0; }
)");
        }

        // The host destructor removes the borrowed width/height declarations, which is a style edit
        // on the root: it marks Style. A real surface destroy does the same thing (the host's
        // root-size buffers die with it), so the settled state this test needs is "settled except
        // for that bookkeeping", and the flags are cleared the way the next frame's end_frame
        // would. Clearing here is what makes case 2 assert the REAL condition - no node has a
        // pending edit - instead of tripping over the harness's own teardown.
        root.clear_subtree_dirty(tessera::ui::Dirty::Style);
        root.clear_dirty(tessera::ui::Dirty::Style);

        // The tree must be clean for this test to mean anything: if the setup host left dirt
        // behind, case 2 would pass for the wrong reason and the regression would go unnoticed.
        // Both flags are asserted: `dirty()` is the per-node flag the damage pass reads, and
        // `subtree_dirty()` is the cached aggregate a parent consults to decide whether to descend.
        if (!expect(root.dirty() == tessera::ui::Dirty::Clean,
                    "the root is settled before the replacement host is built") ||
            !expect(root.subtree_dirty() == tessera::ui::Dirty::Clean,
                    "the root's subtree aggregate is settled too") ||
            !expect(root.children().front()->dirty() == tessera::ui::Dirty::Clean,
                    "the child is settled before the replacement host is built")) {
            return 3;
        }

        // Second host: the surface was recreated. Nothing is dirty anywhere.
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        tessera::app::AppHost host(root, std::move(sheet), loop, config_for());

        const tessera::app::HostFrame frame = host.begin_frame(0.0);
        CountingSink sink;
        const auto paint = host.paint(sink);
        if (!expect(frame.frame_due, "a new host over a settled tree still makes a frame due") ||
            !expect(frame.render.should_render,
                    "a new host over a settled tree still plans a frame") ||
            !expect(frame.render.full_redraw, "a new host's first frame is a full repaint") ||
            !expect(paint.painted, "a new host over a settled tree paints its first frame") ||
            !expect(sink.fills > 0 || sink.texts > 0,
                    "a new host's first frame emits paint commands")) {
            return 4;
        }
        host.end_frame();

        // --- 3. And it is a ONE-shot latch: the next idle frame paints nothing ---------------
        // The fix must not turn every frame into a full repaint, which would defeat damage
        // tracking and present on an idle window (ADR-019).
        const tessera::app::HostFrame idle = host.begin_frame(1.0 / 60.0);
        CountingSink idle_sink;
        const auto idle_paint = host.paint(idle_sink);
        if (!expect(!idle.frame_due, "the frame after the repaint is not due") ||
            !expect(!idle_paint.painted, "the frame after the repaint paints nothing") ||
            !expect(idle_sink.fills == 0 && idle_sink.texts == 0,
                    "an idle frame emits no paint commands")) {
            return 5;
        }
        host.end_frame();
    }

    std::cout << "app_host_repaint: ok\n";
    return 0;
}
