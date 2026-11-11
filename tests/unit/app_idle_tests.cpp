// SPDX-License-Identifier: MIT
//
// Window-driver idle and present policy (docs/design/game-loop.md section 9): park on the pacer
// deadline, never sleep past it, and present only frames whose damage plan has regions. Deadlines
// are driven through FramePacer's explicit `now`, so there is no wall clock and no sleep here.

#include "tessera/app/host.hpp"
#include "tessera/app/idle_policy.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/frame_pacer.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

using tessera::app::HostConfig;
using tessera::app::HostFrame;
using tessera::app::idle_wait_seconds;
using tessera::app::kMaxIdleWaitSeconds;
using tessera::app::pacer_wait_seconds;
using tessera::app::should_present;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::runtime::FramePacer;
using tessera::runtime::FramePacerConfig;
using tessera::runtime::ManualClock;
using tessera::runtime::PaceDecision;
using tessera::runtime::RuntimeLoop;
using tessera::runtime::RuntimeTick;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::TextDraw;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_idle: " << message << "\n";
    }
    return condition;
}

bool near(double lhs, double rhs) {
    return std::fabs(lhs - rhs) <= 1.0e-9;
}
PaintData text_payload(std::string text) {
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::move(text);
    return data;
}

// Counts painted commands so the model can assert zero painting after the scene settles.
class CountingSink final : public PaintSink {
public:
    void fill_rect(const Rect& /*rect*/, const Color& /*color*/) override {
        ++fills;
    }
    void fill_rounded_rect(const Rect& /*rect*/, const Color& /*color*/,
                           float /*radius*/) override {
        ++fills;
    }
    void draw_text(const TextDraw& /*run*/) override {
        ++texts;
    }

    int fills = 0;
    int texts = 0;
};

} // namespace

int main() {
    // --- 1. Uncapped pacing: a present is due immediately, so a present iteration idles nothing,
    //        while a settled scene still parks for the clamp instead of polling. ----------------
    {
        FramePacer uncapped(FramePacerConfig{0.0});
        const PaceDecision decision = uncapped.ready(10.0);
        if (!expect(decision.render_now, "uncapped pacing always has a frame due") ||
            !expect(pacer_wait_seconds(decision) == 0.0, "a due frame does not idle") ||
            !expect(near(idle_wait_seconds(decision), kMaxIdleWaitSeconds),
                    "a settled uncapped scene parks for the clamp") ||
            !expect(near(idle_wait_seconds(decision, 0.05), 0.05), "the clamp is adjustable")) {
            return 1;
        }
    }

    // --- 2. A capped pacer schedules the next deadline exactly one interval out ----------------
    {
        FramePacer pacer(FramePacerConfig{60.0});
        pacer.present(0.0);
        const double interval = pacer.interval_seconds();
        if (!expect(near(interval, 1.0 / 60.0), "60 fps caps the interval at 1/60 s")) {
            return 2;
        }

        const PaceDecision at_present = pacer.ready(0.0);
        if (!expect(!at_present.render_now, "the next frame is not due at the present instant") ||
            !expect(near(at_present.wait_seconds, interval),
                    "the pacer reports the full interval") ||
            !expect(near(pacer_wait_seconds(at_present), interval),
                    "the idle budget is the deadline") ||
            !expect(near(idle_wait_seconds(at_present), interval),
                    "a settled capped scene waits for its deadline")) {
            return 3;
        }

        const double half = interval * 0.5;
        const PaceDecision mid = pacer.ready(half);
        if (!expect(near(pacer_wait_seconds(mid), half), "the budget shrinks with elapsed time") ||
            !expect(!mid.render_now, "mid-interval is not due")) {
            return 4;
        }

        // Never sleep past the deadline: the budget can only shorten the remaining time.
        const PaceDecision at_deadline = pacer.ready(interval);
        if (!expect(at_deadline.render_now, "the deadline makes the frame due") ||
            !expect(pacer_wait_seconds(at_deadline) == 0.0, "a due frame never sleeps") ||
            !expect(near(idle_wait_seconds(at_deadline), kMaxIdleWaitSeconds),
                    "after the deadline a settled scene parks for the clamp")) {
            return 5;
        }

        const PaceDecision late = pacer.ready(interval * 3.0);
        if (!expect(late.render_now && late.late, "a missed deadline stays due") ||
            !expect(pacer_wait_seconds(late) == 0.0, "a late frame does not sleep further")) {
            return 6;
        }
    }

    // --- 3. The clamp bounds a long deadline and rejects degenerate budgets --------------------
    {
        FramePacer slow(FramePacerConfig{1.0});
        slow.present(0.0); // the next deadline is one second out
        const PaceDecision decision = slow.ready(0.0);
        if (!expect(near(pacer_wait_seconds(decision, 0.05), 0.05), "a long deadline is clamped") ||
            !expect(near(idle_wait_seconds(decision, 0.05), 0.05), "the idle clamp applies too") ||
            !expect(pacer_wait_seconds(decision, -1.0) == 0.0,
                    "a negative clamp degrades to zero") ||
            !expect(idle_wait_seconds(decision, 0.0) == 0.0, "a zero clamp degrades to zero")) {
            return 7;
        }
        const PaceDecision nan_wait{false, std::nan(""), false};
        const PaceDecision infinite_wait{false, HUGE_VAL, false};
        if (!expect(pacer_wait_seconds(nan_wait) == 0.0, "a non-finite wait is dropped") ||
            !expect(near(pacer_wait_seconds(infinite_wait), kMaxIdleWaitSeconds),
                    "an infinite wait is clamped")) {
            return 8;
        }
    }

    // --- 4. Present only changed frames; the forced CI path is the exception -------------------
    {
        if (!expect(!should_present(false, false, false), "a clean frame is not presented") ||
            !expect(!should_present(false, true, false), "a stale damage flag does not present") ||
            !expect(!should_present(true, false, false), "a due frame without damage is skipped") ||
            !expect(should_present(true, true, false), "a due damaged frame is presented") ||
            !expect(should_present(false, false, true), "forced frames paint a clean scene") ||
            !expect(should_present(true, true, true), "forcing keeps a damaged frame")) {
            return 9;
        }
    }

    // --- 5. A settled scene: every iteration parks a positive, bounded time and presents nothing
    {
        FramePacer pacer(FramePacerConfig{60.0});
        pacer.present(0.0);
        double now = 0.0;
        double total_idle = 0.0;
        int presents = 0;
        for (int iteration = 0; iteration < 240; ++iteration) {
            const PaceDecision decision = pacer.ready(now);
            const double wait = idle_wait_seconds(decision);
            if (!expect(wait > 0.0 && wait <= kMaxIdleWaitSeconds,
                        "an idle iteration never spins and never exceeds the clamp")) {
                return 10;
            }
            if (!decision.render_now &&
                !expect(wait <= decision.wait_seconds,
                        "an idle iteration never sleeps past the deadline")) {
                return 10;
            }
            if (should_present(false, false, false)) {
                ++presents;
            }
            now += wait;
            total_idle += wait;
        }
        if (!expect(presents == 0, "a settled scene presents nothing") ||
            !expect(total_idle > 0.5, "the model actually idled through the script")) {
            return 11;
        }

        // One changed frame presents exactly once, then the scene settles again.
        if (!expect(should_present(true, true, false), "the changed frame is due")) {
            return 12;
        }
        ++presents;
        for (int iteration = 0; iteration < 120; ++iteration) {
            if (!expect(!should_present(false, false, false), "the settled scene stays clean")) {
                return 12;
            }
        }
        if (!expect(presents == 1, "exactly one present for one change")) {
            return 13;
        }
    }

    // --- 6. The policy drives real host damage: the first frame presents, a settled scene does no
    //        further paint/present work, and one label change is exactly one more frame. ---------
    {
        Node root(1);
        auto label_ptr = std::make_unique<Node>(2);
        Node& label = root.add_child(std::move(label_ptr));
        label.set_element_kind("Text");
        label.set_paint_data(text_payload("static"));

        ManualClock clock;
        RuntimeLoop loop(clock);
        loop.mark_render_dirty();
        HostConfig config;
        config.width = 200.0f;
        config.height = 120.0f;
        tessera::app::AppHost host(
            root, tessera::ui::style::compile_stylesheet("Text { font-size: 12px; }"), loop,
            config);
        FramePacer pacer(FramePacerConfig{60.0});

        int paints = 0;
        int presents = 0;
        // One iteration of the driver's decision sequence: dirty scan -> tick -> pacer gate ->
        // host frame -> present-or-end_frame. The window driver's idle and retry paths add only the
        // platform wait, which this model skips.
        const auto drive = [&](bool force) {
            if (root.subtree_dirty() != Dirty::Clean || root.dirty() != Dirty::Clean) {
                loop.mark_render_dirty();
            }
            const RuntimeTick tick = loop.tick();
            if (!tick.should_render || !pacer.ready(clock.now_seconds()).render_now) {
                return;
            }
            const HostFrame frame = host.begin_frame(1.0 / 60.0, tick.alpha);
            const bool damage = !frame.render.redraw_plan.scissors.empty();
            if (!should_present(frame.frame_due, damage, force)) {
                host.end_frame();
                loop.clear_render_dirty();
                return;
            }
            CountingSink sink;
            (void)host.paint(sink);
            if (sink.fills + sink.texts > 0) {
                ++paints;
            }
            host.end_frame();
            loop.clear_render_dirty();
            pacer.present(clock.now_seconds());
            ++presents;
        };

        drive(false);
        if (!expect(presents == 1 && paints == 1, "the first frame is painted and presented")) {
            return 14;
        }
        for (int iteration = 0; iteration < 60; ++iteration) {
            clock.advance(1.0 / 60.0);
            drive(false);
        }
        if (!expect(presents == 1, "a settled scene presents nothing") ||
            !expect(paints == 1, "a settled scene paints nothing")) {
            return 15;
        }

        label.set_paint_data(text_payload("changed"));
        clock.advance(1.0 / 60.0);
        drive(false);
        if (!expect(presents == 2 && paints == 2, "one change is exactly one painted present")) {
            return 16;
        }
        for (int iteration = 0; iteration < 60; ++iteration) {
            clock.advance(1.0 / 60.0);
            drive(false);
        }
        if (!expect(presents == 2 && paints == 2, "the scene settles again")) {
            return 17;
        }
    }

    std::cout << "app_idle: ok\n";
    return 0;
}
