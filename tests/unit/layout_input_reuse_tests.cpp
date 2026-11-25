// SPDX-License-Identifier: MIT
//
// The retained layout input's reuse (layout_input_reuse.hpp): a pass skips a subtree with no style,
// layout or paint dirt, so anything that changes the scene without the pass seeing it has to make
// the next pass rebuild the whole input. The randomized layout_gate test covers the edits that mark
// layout; this one covers the case that does not: a text payload changed with Paint dirt only, in
// a frame whose layout pass never ran, followed by a frame that does lay out. A later layout must
// still read the new text, exactly like a host that never kept an input.
//
// The check is the same as layout_gate's: the warm host's boxes against a host that lays out the
// final tree cold. A reused subtree that kept the old text shows up as a label with the old width.

#include "layout_gate_scene.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

using tessera::test::layout_gate::Boxes;
using tessera::test::layout_gate::clone_tree;
using tessera::test::layout_gate::collect_boxes;
using tessera::test::layout_gate::Scene;
using tessera::ui::style::compile_stylesheet;

// Labels have a fixed width and no height, and wrap, so their height is the measured line count: a
// text change that a pass never read is visible in a box.
constexpr const char* kSheet =
    R"(.card { width: 300px; height: 40px; padding: 4px; flex-shrink: 0; background-color: #1e293b; }
.card.hot { background-color: #334155; }
.card.wide { width: 360px; }
Text { width: 100px; font-size: 14px; color: #e2e8f0; flex-shrink: 0; }
)";

struct NullSink final : tessera::ui::paint::PaintSink {
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {}
    void draw_text(const tessera::ui::paint::TextDraw&) override {}
};

tessera::ui::PaintData text_payload(std::string text) {
    tessera::ui::PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = std::move(text);
    data.wrap = tessera::ui::PaintWrap::Wrap;
    return data;
}

bool same(const Boxes& a, const Boxes& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first) {
            return false;
        }
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::fabs(a[i].second[c] - b[i].second[c]) > 0.001f) {
                return false;
            }
        }
    }
    return true;
}

Boxes cold_layout(const tessera::ui::Node& tree) {
    std::unique_ptr<tessera::ui::Node> root = clone_tree(tree);
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 800.0f;
    config.height = 600.0f;
    tessera::app::AppHost cold(*root, compile_stylesheet(kSheet), loop, config);
    (void)cold.begin_frame(0.0);
    Boxes boxes;
    collect_boxes(*root, boxes);
    return boxes;
}

// Frame 1 changes label 0's text with Paint dirt only (and, if `restyle`, a paint-only class on a
// card so the host does run a restyle and the layout gate accepts it); frame 2 moves a box on
// another card, so the host lays out. Returns whether frame 2 equals a cold layout.
bool run(bool restyle, const char* name) {
    Scene scene(4);
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 800.0f;
    config.height = 600.0f;
    tessera::app::AppHost host(*scene.root, compile_stylesheet(kSheet), loop, config);
    NullSink sink;
    const auto frame = [&] {
        (void)host.begin_frame(1.0 / 60.0);
        (void)host.paint(sink);
        host.end_frame();
    };
    for (tessera::ui::Node* label : scene.labels) {
        label->set_paint_data(text_payload("short"));
    }
    frame(); // frame 0: the first layout builds the retained input

    const std::optional<tessera::ui::PaintData> short_text = scene.labels[0]->paint_data();
    scene.labels[0]->set_paint_data(text_payload(
        "a considerably longer label than the one the first layout measured, which wraps"));
    if (restyle) {
        scene.cards[0]->set_class_name("card hot");
    }
    frame(); // frame 1: no layout pass reads the new text

    scene.cards[1]->set_class_name("card wide");
    frame(); // frame 2: a layout pass runs and must read it

    Boxes warm;
    collect_boxes(*scene.root, warm);
    const Boxes cold = cold_layout(*scene.root);
    // Premise: with the original text restored, a cold layout of the same tree differs, so the text
    // is something a box can show.
    const std::optional<tessera::ui::PaintData> long_text = scene.labels[0]->paint_data();
    scene.labels[0]->set_paint_data(short_text);
    const Boxes cold_short = cold_layout(*scene.root);
    scene.labels[0]->set_paint_data(long_text);
    if (same(cold_short, cold)) {
        std::cerr << "layout_input_reuse: " << name
                  << ": the text does not reach any box, so this test would prove nothing\n";
        return false;
    }
    if (!same(warm, cold)) {
        std::cerr << "layout_input_reuse: " << name
                  << ": the layout after an unseen text change differs from a cold layout\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    const bool plain = run(false, "no restyle");
    const bool gated = run(true, "gate-accepted restyle");
    if (!plain || !gated) {
        return 1;
    }
    std::cout << "layout_input_reuse: ok\n";
    return 0;
}
