// SPDX-License-Identifier: MIT
//
// The layout-unchanged gate's acceptance test (issue #735, slice A): a frame whose layout pass the
// gate skipped must be byte-identical to the same frame laid out cold. The gate's whole claim is
// "every layout input is unchanged, so the previous result is still what a reflow would produce",
// and the only honest way to check that claim is to compare against a reflow.
//
// The test drives a randomized edit sequence over a tree that exercises every layout input source
// the gate has to cover:
//   * layout-affecting longhands (width, padding, margin, display, font-size);
//   * paint-only longhands (background-color, border-radius) - these must NOT move a box, so the
//     gate must accept them;
//   * text, which reaches Yoga through the measure callback;
//   * the tree shape (adding and removing a child), which no computed style carries;
//   * the viewport (a resize), which no computed style carries either.
//
// After every frame the scene's boxes are compared against a second host that has never seen the
// edit history: it is constructed fresh on an independent copy of the same tree and lays out once.
// A gate that accepted a frame it should have declined shows up as a box that differs from the cold
// layout.
//
// Two passes, because the cold reference cannot share a frame with the warm host: building a second
// tree bumps the process-wide structure epoch, which the warm host would read as a shape change on
// the *next* frame and decline the gate for the wrong reason. Pass 1 drives the warm host and
// records every frame's boxes; pass 2 replays the same edit sequence and compares each frame
// against a cold layout. Both passes use the same seed, so the edit sequence is identical.
//
// Deterministic: the "random" sequence is a fixed-seed LCG, so a failure is reproducible.
//
// The scene and edit machinery live in `layout_gate_scene.{hpp,cpp}`; this file is the driver.

#include "layout_gate_scene.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::test::layout_gate::apply_edit;
using tessera::test::layout_gate::Boxes;
using tessera::test::layout_gate::clone_tree;
using tessera::test::layout_gate::collect_boxes;
using tessera::test::layout_gate::Edit;
using tessera::test::layout_gate::kEditKindCount;
using tessera::test::layout_gate::kSheet;
using tessera::test::layout_gate::Lcg;
using tessera::test::layout_gate::moves_a_box;
using tessera::test::layout_gate::name_of;
using tessera::test::layout_gate::Scene;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "layout_gate: " << message << "\n";
    }
    return condition;
}

// A sink that records nothing: this test is about boxes, not pixels.
struct NullSink final : tessera::ui::paint::PaintSink {
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {}
    void draw_text(const tessera::ui::paint::TextDraw&) override {}
};

} // namespace

int main() {
    // Six cards, not twelve: at 52 logical px each (40 content + 8 padding + 4 margin) that is 312
    // px, which fits inside the smaller of the two viewports (480 px). Nothing overflows, so no
    // flex item is ever shrunk and every box is a pure function of the sheet.
    constexpr int kCards = 6;
    constexpr int kFrames = 240;

    // --- pass 1: the warm host ---------------------------------------------------------------
    std::vector<Boxes> warm_boxes;
    std::vector<bool> gate_accepted;
    std::size_t gate_hits = 0;
    std::size_t gate_declines = 0;
    std::size_t accepted_paint_edits = 0;
    std::size_t declined_layout_edits = 0;
    {
        Scene scene(kCards);
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        tessera::app::HostConfig config;
        config.width = 800.0f;
        config.height = 600.0f;
        tessera::app::AppHost host(*scene.root, compile_stylesheet(kSheet), loop, config);
        NullSink sink;

        Lcg rng(0x7350ULL);
        float width = config.width;
        float height = config.height;
        for (int frame = 0; frame < kFrames; ++frame) {
            const Edit edit = static_cast<Edit>(rng.below(kEditKindCount));
            float requested_width = width;
            float requested_height = height;
            apply_edit(edit, scene, rng, frame, requested_width, requested_height);
            if (requested_width != width || requested_height != height) {
                width = requested_width;
                height = requested_height;
                host.resize(width, height, 1.0f);
            }

            const std::size_t gate_before = host.layout_gate_hits();
            const auto hf = host.begin_frame(1.0 / 60.0);
            (void)host.paint(sink);
            host.end_frame();
            const bool skipped = host.layout_gate_hits() > gate_before;
            skipped ? ++gate_hits : ++gate_declines;

            // The gate's contract, checked only on frames that actually reached it. A frame with no
            // style work never runs a restyle, so it has no layout decision to make: `class-paint`
            // on a card that is already `hot` is a no-op edit, and the host correctly does nothing.
            if (hf.style_rebuilt) {
                if (!moves_a_box(edit)) {
                    if (!expect(skipped, std::string("a paint-only edit (") + name_of(edit) +
                                             ") must be accepted by the gate")) {
                        std::cerr << "  declined because: " << host.layout_gate_reason() << "\n";
                        return 4;
                    }
                    ++accepted_paint_edits;
                } else {
                    if (!expect(!skipped, std::string("a layout edit (") + name_of(edit) +
                                              ") must be declined by the gate")) {
                        std::cerr << "  frame " << frame << " accepted a layout edit\n";
                        return 5;
                    }
                    ++declined_layout_edits;
                }
            }

            // Recorded for every frame, so the two passes stay index-aligned.
            Boxes boxes;
            collect_boxes(*scene.root, boxes);
            warm_boxes.push_back(std::move(boxes));
            gate_accepted.push_back(skipped);
        }
    }

    // --- pass 2: the cold reference -----------------------------------------------------------
    {
        Scene scene(kCards);
        Lcg rng(0x7350ULL);
        float width = 800.0f;
        float height = 600.0f;
        for (int frame = 0; frame < kFrames; ++frame) {
            const Edit edit = static_cast<Edit>(rng.below(kEditKindCount));
            float requested_width = width;
            float requested_height = height;
            apply_edit(edit, scene, rng, frame, requested_width, requested_height);
            width = requested_width;
            height = requested_height;

            // A host that has never seen this edit history, on an independent copy of the same
            // style inputs. It lays out once from scratch, which is exactly what a reflow of the
            // current inputs produces.
            std::unique_ptr<tessera::ui::Node> cold_root = clone_tree(*scene.root);
            tessera::runtime::ManualClock cold_clock;
            tessera::runtime::RuntimeLoop cold_loop(cold_clock);
            tessera::app::HostConfig cold_config;
            cold_config.width = width;
            cold_config.height = height;
            tessera::app::AppHost cold(*cold_root, compile_stylesheet(kSheet), cold_loop,
                                       cold_config);
            (void)cold.begin_frame(0.0);

            Boxes cold_boxes;
            collect_boxes(*cold_root, cold_boxes);
            const Boxes& warm = warm_boxes[static_cast<std::size_t>(frame)];
            if (warm.size() != cold_boxes.size()) {
                std::cerr << "layout_gate: frame " << frame << " (" << name_of(edit)
                          << ") box count " << warm.size() << " but a cold layout gives "
                          << cold_boxes.size() << "\n";
                return 1;
            }
            for (std::size_t i = 0; i < warm.size(); ++i) {
                if (warm[i].first != cold_boxes[i].first) {
                    std::cerr << "layout_gate: frame " << frame << " (" << name_of(edit)
                              << ") node order differs at " << i << ": " << warm[i].first << " vs "
                              << cold_boxes[i].first << "\n";
                    return 1;
                }
                for (std::size_t c = 0; c < 4; ++c) {
                    if (std::fabs(warm[i].second[c] - cold_boxes[i].second[c]) > 0.001f) {
                        std::cerr << "layout_gate: frame " << frame << " (" << name_of(edit)
                                  << ", gate "
                                  << (gate_accepted[static_cast<std::size_t>(frame)] ? "accepted"
                                                                                     : "declined")
                                  << ") node " << warm[i].first << " box component " << c << " is "
                                  << warm[i].second[c] << " but a cold layout gives "
                                  << cold_boxes[i].second[c] << "\n";
                        return 1;
                    }
                }
            }
        }
    }

    // The gate must actually be exercised in both directions, or this test proves nothing.
    if (!expect(gate_hits > 0, "the gate accepted at least one frame") ||
        !expect(gate_declines > 0, "the gate declined at least one frame") ||
        !expect(accepted_paint_edits > 0, "at least one paint-only edit was accepted") ||
        !expect(declined_layout_edits > 0, "at least one layout edit was declined")) {
        return 2;
    }
    std::cout << "layout_gate: " << kFrames << " frames, gate accepted " << gate_hits << " ("
              << accepted_paint_edits << " paint-only edits) and declined " << gate_declines << " ("
              << declined_layout_edits << " layout edits), every box matched a cold layout\n";
    return 0;
}
