// SPDX-License-Identifier: MIT
//
// Measurement and regression guard for issue #200 ("skip the dirty-driven present for held
// animations"), which asked for a measurement before any code: does an animation that holds a steady
// value keep repainting its node every tick while another animation keeps the loop awake? It did:
// 100 held fills turned a 2-node, 1 627-pixel frame into a 102-node, 921 600-pixel (full redraw)
// frame on every tick. Held fills no longer mark their node dirty, and this fails if they do again.
//
// Scene: one mover (an infinite transform animation, so the loop never sleeps) and N holders, each a
// short opacity animation with `animation-fill-mode: forwards`. After the holders finish, their
// final value is held by the runtime's fill list and sampled every tick. The probe prints, per frame
// after the holds are reached, how many nodes the paint resolved and how many device pixels the
// redraw plan covered, for N = 0 (the mover alone) and N holders. The difference is the cost of the
// held animations. Deterministic: ManualClock, no GPU, no font, no wall clock.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

using tessera::ui::Node;

class NullSink final : public tessera::ui::paint::PaintSink {
public:
    void fill_rect(const tessera::rendering::rhi::Rect&, const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&, const tessera::rendering::rhi::Color&,
                           float) override {}
    void draw_text(const tessera::ui::paint::TextDraw&) override {}
};

constexpr const char* kSheet = R"(
@keyframes slide { from { transform: translateX(0px); } to { transform: translateX(40px); } }
@keyframes fade { from { opacity: 0.5; } to { opacity: 1; } }
.mover { width: 40px; height: 40px; background-color: #334155; animation: slide 1s linear infinite; }
.holder { width: 20px; height: 20px; margin: 2px; background-color: #1e293b;
          animation: fade 0.05s linear forwards; }
)";

struct Result {
    double mean_nodes = 0.0;
    double mean_damage_pixels = 0.0;
    double presented_fraction = 0.0;
};

Result measure(int holders) {
    auto root = std::make_unique<Node>(1);
    Node& mover = root->add_child(std::make_unique<Node>(2));
    mover.set_element_kind("View");
    mover.set_class_name("mover");
    for (int i = 0; i < holders; ++i) {
        Node& holder = root->add_child(std::make_unique<Node>(static_cast<std::uint64_t>(3 + i)));
        holder.set_element_kind("View");
        holder.set_class_name("holder");
    }
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 1280.0f;
    config.height = 720.0f;
    tessera::app::AppHost host(*root, tessera::ui::style::compile_stylesheet(kSheet), loop, config);
    NullSink sink;

    constexpr int kWarmup = 30; // 0.5 s: every holder finished (0.05 s) and is held
    constexpr int kFrames = 60;
    double nodes = 0.0;
    double pixels = 0.0;
    int presented = 0;
    for (int frame = 0; frame < kWarmup + kFrames; ++frame) {
        const tessera::app::HostFrame result = host.begin_frame(1.0 / 60.0);
        std::size_t painted_nodes = 0;
        if (result.frame_due) {
            painted_nodes = host.paint(sink).stats.nodes;
            host.end_frame();
        }
        if (frame < kWarmup) {
            continue;
        }
        if (result.frame_due) {
            ++presented;
        }
        nodes += static_cast<double>(painted_nodes);
        for (const auto& scissor : result.render.redraw_plan.scissors) {
            pixels += static_cast<double>(scissor.width) * static_cast<double>(scissor.height);
        }
    }
    return {nodes / kFrames, pixels / kFrames, static_cast<double>(presented) / kFrames};
}

} // namespace

int main(int argc, char** argv) {
    const int holders = argc > 1 ? std::atoi(argv[1]) : 100;
    const Result alone = measure(0);
    const Result held = measure(holders);
    std::printf("held_animation_probe: mover alone      nodes/frame=%8.1f damage_px/frame=%10.0f presented=%.2f\n",
                alone.mean_nodes, alone.mean_damage_pixels, alone.presented_fraction);
    std::printf("held_animation_probe: + %-4d held      nodes/frame=%8.1f damage_px/frame=%10.0f presented=%.2f\n",
                holders, held.mean_nodes, held.mean_damage_pixels, held.presented_fraction);
    // The held nodes must cost nothing: the same nodes painted per frame as the mover alone.
    if (held.mean_nodes > alone.mean_nodes + 0.5) {
        std::fprintf(stderr, "held_animation_probe: held fills repaint their nodes every tick\n");
        return 1;
    }
    return 0;
}
