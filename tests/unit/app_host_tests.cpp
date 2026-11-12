// SPDX-License-Identifier: MIT
//
// AppHost headless contract (docs/design/app-host.md section 3): the host runs tick -> style ->
// layout -> scene sync -> damage -> paint for one scene root, and a clean frame does no geometry or
// paint work at all. The driver here is a ManualClock + RuntimeLoop, exactly like the windowed
// runner, so the assertions are deterministic.

#include "tessera/app/host.hpp"
#include "tessera/app/system_motion.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::has_flag;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_host: " << message << "\n";
    }
    return condition;
}

bool same_color(const Color& lhs, const Color& rhs) {
    const auto near = [](float a, float b) { return std::fabs(a - b) <= 0.01f; };
    return near(lhs.r, rhs.r) && near(lhs.g, rhs.g) && near(lhs.b, rhs.b) && near(lhs.a, rhs.a);
}

// Records the flat paint commands so the test can assert what the host painted without a GPU.
class RecordingSink final : public tessera::ui::paint::PaintSink {
public:
    struct Fill {
        Rect rect;
        Color color;
    };

    void fill_rect(const Rect& rect, const Color& color) override {
        fills.push_back({rect, color});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        fills.push_back({rect, color});
    }
    void draw_text(const TextDraw& run) override {
        texts.emplace_back(run.text);
    }

    bool has_text(const std::string& wanted) const {
        for (const std::string& text : texts) {
            if (text == wanted) {
                return true;
            }
        }
        return false;
    }

    std::vector<Fill> fills;
    std::vector<std::string> texts;
};

PaintData text_payload(std::string text) {
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::move(text);
    return data;
}

} // namespace

int main() {
    Node root(1);
    auto card_ptr = std::make_unique<Node>(2);
    Node& card = root.add_child(std::move(card_ptr));
    card.set_element_kind("View");
    card.set_class_name("card");

    auto label_ptr = std::make_unique<Node>(3);
    Node& label = card.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    label.set_paint_data(text_payload("Count: 0"));

    auto sheet = compile_stylesheet(R"(
.card { width: 200px; height: 100px; padding: 8px; background-color: #1e293b; }
Text { font-size: 16px; color: #e2e8f0; }
)");

    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);

    tessera::app::HostConfig config;
    config.width = 320.0f;
    config.height = 200.0f;
    tessera::app::AppHost host(root, std::move(sheet), loop, config);

    // --- 1. First frame: style, layout, damage and a paint --------------------------------
    tessera::app::HostFrame first = host.begin_frame(0.0);
    if (!expect(first.style_rebuilt, "first frame rebuilds the selector tree") ||
        !expect(first.tick.needs_style, "first frame reports style work") ||
        !expect(first.tick.needs_layout, "first frame reports layout work") ||
        !expect(first.frame_due, "first frame is due") ||
        !expect(first.layout.applied >= 3, "layout covered the root, card and label") ||
        !expect(first.render.should_render, "first frame has damage")) {
        return 1;
    }
    // CSS `box-sizing` is content-box by default, so the card's 200x100 content plus 8 px padding
    // on every side is a 216x116 border box.
    if (!expect(root.world_bounds().width == 320.0f, "root takes the window width") ||
        !expect(card.world_bounds().width == 216.0f, "class selector sizes the card") ||
        !expect(card.world_bounds().height == 116.0f, "class selector sets the card height")) {
        return 2;
    }

    RecordingSink first_sink;
    const auto first_paint = host.paint(first_sink);
    if (!expect(first_paint.painted, "first frame painted") ||
        !expect(first_sink.has_text("Count: 0"), "generated text painted") ||
        !expect(!first_sink.fills.empty(), "card background painted") ||
        !expect(same_color(first_sink.fills.front().color,
                           Color{30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 1.0f}),
                "card uses the stylesheet background color")) {
        return 3;
    }
    host.end_frame();
    if (!expect(root.dirty() == Dirty::Clean, "end_frame clears the root dirty flags") ||
        !expect(!has_flag(label.dirty(), Dirty::Paint), "end_frame clears the label dirty flags")) {
        return 4;
    }

    // --- 2. Idle: a clean frame does no work and paints nothing ---------------------------
    const tessera::app::HostFrame idle = host.begin_frame(1.0 / 60.0);
    RecordingSink idle_sink;
    const auto idle_paint = host.paint(idle_sink);
    if (!expect(!idle.tick.needs_style && !idle.tick.needs_layout,
                "idle frame has no geometry work") ||
        !expect(!idle.style_rebuilt, "idle frame does not rebuild the selector tree") ||
        !expect(!idle.frame_due, "idle frame is not due") ||
        !expect(!idle_paint.painted, "idle frame paints nothing") ||
        !expect(idle_sink.fills.empty() && idle_sink.texts.empty(), "idle sink is empty")) {
        return 5;
    }

    // --- 3. A paint-only inline style edit hits the layout signature guard ----------------
    label.set_inline_style("background", "#ff0000");
    const std::size_t style_runs = host.layout_runs();
    const std::size_t style_hits = host.layout_cache_hits();
    const tessera::app::HostFrame restyled = host.begin_frame(1.0 / 60.0);
    if (!expect(restyled.tick.needs_style, "a style-only edit reports style work") ||
        !expect(!restyled.tick.needs_layout, "a paint-only edit reports no layout dirt") ||
        !expect(host.layout_runs() == style_runs,
                "a paint-only inline style edit must not run a layout pass") ||
        !expect(host.layout_cache_hits() == style_hits + 1,
                "a paint-only inline style edit is a layout cache hit")) {
        return 20;
    }
    host.end_frame();

    // --- 4. State change: paint-only frame with partial damage ----------------------------
    label.set_paint_data(text_payload("Count: 1"));
    const tessera::app::HostFrame changed = host.begin_frame(1.0 / 60.0);
    if (!expect(!changed.tick.needs_style && !changed.tick.needs_layout,
                "a text-only state change needs no style or layout work") ||
        !expect(changed.frame_due, "state change makes a frame due") ||
        !expect(changed.render.damage.rects >= 1, "state change produces damage") ||
        !expect(!changed.render.full_redraw, "a small text change is a partial redraw") ||
        !expect(!changed.render.redraw_plan.scissors.empty(), "partial plan has scissor regions")) {
        return 6;
    }
    RecordingSink changed_sink;
    const auto changed_paint = host.paint(changed_sink);
    if (!expect(changed_paint.painted, "state change painted") ||
        !expect(changed_sink.has_text("Count: 1"), "the new text is painted")) {
        return 7;
    }
    host.end_frame();

    host.resize(400.0f, 300.0f, 2.0f);
    const auto extent = host.target_extent();
    if (!expect(extent.width == 800 && extent.height == 600, "damage target follows DPI scale")) {
        return 8;
    }
    // --- 5. Resize: new target, reflow, full repaint --------------------------------------
    const tessera::app::HostFrame resized = host.begin_frame(1.0 / 60.0);
    if (!expect(resized.tick.needs_layout, "resize asks for layout") ||
        !expect(resized.frame_due, "resize makes a frame due") ||
        !expect(root.world_bounds().width == 400.0f, "root takes the resized width") ||
        !expect(root.world_bounds().height == 300.0f, "root takes the resized height") ||
        !expect(!resized.render.redraw_plan.scissors.empty(), "resize repaints")) {
        return 9;
    }
    host.end_frame();

    // --- 6. Back to idle after the resize frame -------------------------------------------
    const tessera::app::HostFrame settled = host.begin_frame(1.0 / 60.0);
    if (!expect(!settled.frame_due, "the loop settles again")) {
        return 10;
    }

    // --- 7. The guard cannot silently skip a real layout input change ----------------------
    // A hit is only sound for an unchanged input, so each edit the engine consumes must still
    // reach Yoga exactly once.
    {
        const std::size_t runs = host.layout_runs();
        card.set_inline_style("width", "240px");
        (void)host.begin_frame(1.0 / 60.0);
        host.end_frame();
        if (!expect(host.layout_runs() == runs + 1,
                    "a width change runs exactly one layout pass") ||
            !expect(card.world_bounds().width == 256.0f,
                    "the width change reaches the card's box")) {
            return 21;
        }
    }
    {
        // A text change raises layout dirt through the conservative caller path; the signature
        // must see the new bytes and reflow instead of trusting the dirt or dropping it.
        const std::size_t runs = host.layout_runs();
        label.set_paint_data(text_payload("Count: 2"));
        label.mark_dirty(Dirty::Layout);
        (void)host.begin_frame(1.0 / 60.0);
        host.end_frame();
        if (!expect(host.layout_runs() == runs + 1, "a text change runs exactly one layout pass")) {
            return 22;
        }
    }
    {
        const std::size_t runs = host.layout_runs();
        host.resize(360.0f, 240.0f, 1.0f);
        (void)host.begin_frame(1.0 / 60.0);
        host.end_frame();
        if (!expect(host.layout_runs() == runs + 1,
                    "a viewport change runs exactly one layout pass") ||
            !expect(root.world_bounds().width == 360.0f,
                    "the viewport change reaches the root's box")) {
            return 23;
        }
    }

    // --- 8. The installed update scheduler flushes at the start of a frame ------------------
    {
        struct Counter {
            int runs = 0;
        };
        Counter counter;
        tessera::ui::UpdateScheduler scheduler;
        tessera::ui::UpdateTicket ticket;
        ticket.update = [](void* context) { static_cast<Counter*>(context)->runs += 1; };
        ticket.context = &counter;
        scheduler.attach(ticket);
        host.set_update_scheduler(&scheduler);
        ticket.schedule();
        if (!expect(ticket.queued(), "the ticket is queued before the frame")) {
            return 11;
        }
        (void)host.begin_frame(1.0 / 60.0);
        host.end_frame();
        host.set_update_scheduler(nullptr);
        scheduler.detach(ticket);
        if (!expect(counter.runs == 1, "the host flushed the scheduled update once") ||
            !expect(!ticket.queued(), "the flushed ticket is no longer queued")) {
            return 11;
        }
    }

    // --- 8b. Reduced motion reaches the animation runtime and the update scheduler -----------
    {
        Node motion_root(1);
        tessera::runtime::ManualClock motion_clock;
        tessera::runtime::RuntimeLoop motion_loop(motion_clock);
        tessera::app::HostConfig motion_config = config;
        motion_config.reduced_motion = true;
        tessera::app::AppHost motion_host(motion_root, compile_stylesheet(""), motion_loop,
                                          motion_config);
        tessera::ui::UpdateScheduler scheduler;
        if (!expect(motion_host.reduced_motion() && motion_host.animations().reduced_motion(),
                    "the config preference reaches the animation runtime")) {
            return 13;
        }
        motion_host.set_update_scheduler(&scheduler);
        if (!expect(scheduler.reduced_motion(), "a scheduler installed later inherits it")) {
            return 13;
        }
        motion_host.set_reduced_motion(false);
        if (!expect(!motion_host.reduced_motion() && !motion_host.animations().reduced_motion() &&
                        !scheduler.reduced_motion(),
                    "a runtime change reaches the runtime and the scheduler")) {
            return 13;
        }
        motion_host.set_update_scheduler(nullptr);
        // The OS query must be callable on every platform; its value depends on the machine.
        (void)tessera::app::system_prefers_reduced_motion();
    }

    // --- 9. A caller-styled root keeps its layout mode -----------------------------------
    {
        Node styled(1);
        styled.set_inline_style("display", "flex");
        styled.set_inline_style("flex-direction", "row");
        auto left_ptr = std::make_unique<Node>(2);
        Node& left = styled.add_child(std::move(left_ptr));
        auto right_ptr = std::make_unique<Node>(3);
        Node& right = styled.add_child(std::move(right_ptr));
        left.set_inline_style("width", "80px");
        left.set_inline_style("height", "20px");
        right.set_inline_style("width", "80px");
        right.set_inline_style("height", "20px");

        tessera::runtime::ManualClock styled_clock;
        tessera::runtime::RuntimeLoop styled_loop(styled_clock);
        tessera::app::AppHost styled_host(styled, compile_stylesheet(""), styled_loop, config);
        (void)styled_host.begin_frame(0.0);
        if (!expect(left.world_bounds().x == 0.0f, "first row child starts at the origin") ||
            !expect(right.world_bounds().x == 80.0f, "second row child follows it horizontally") ||
            !expect(right.world_bounds().y == left.world_bounds().y, "row children share a y")) {
            return 12;
        }
    }

    std::cout << "app_host: ok\n";
    return 0;
}
