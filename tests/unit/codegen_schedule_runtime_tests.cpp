// SPDX-License-Identifier: MIT

// Issue #5 end to end on the generated Sample component: a setter schedules the component, the
// scheduler's flush runs update() once, and nothing repaints without a flush. An async completion
// that sets state repaints on the next frame with no manual update().

#include "Sample.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <iostream>
#include <string_view>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string label_text(ui::Node& root) {
    ui::Node& view = *root.children()[0];
    ui::Node& label = *view.children()[0]->children()[0];
    return label.paint_data() ? label.paint_data()->text : std::string("<none>");
}

struct Wakes {
    int count = 0;
};

void wake(void* context) {
    ++static_cast<Wakes*>(context)->count;
}

} // namespace

int main() {
    ui::Node root(1);
    demo::gen::Sample sample;
    sample.build(root);
    ui::UpdateScheduler scheduler;
    Wakes wakes;
    scheduler.set_wake(&wake, &wakes);
    sample.attach_scheduler(scheduler);

    check(label_text(root) == "Count: 0", "initial text");

    // A setter only schedules: the frame has not run, so the tree is still the old text.
    sample.set_count(3);
    check(sample.state_count_ == 3, "the setter assigns the cell immediately");
    check(label_text(root) == "Count: 0", "scheduling does not repaint inline");
    check(wakes.count == 1, "the setter woke the loop");

    // Equality-gated: setting the value the cell already holds schedules nothing (ADR-041).
    sample.set_count(3);
    check(wakes.count == 1, "an unchanged value does not wake the loop again");
    ui::UpdateFlushStats idle = scheduler.flush();
    check(idle.updated == 1, "the earlier real change still flushed once");
    sample.set_count(3);
    check(!scheduler.pending(), "setting an equal value schedules no update");
    check(scheduler.flush().updated == 0, "an equal-value set flushes nothing");
    sample.set_count(3);
    sample.set_count(4);
    check(scheduler.pending(), "a different value schedules again");

    // Several setters in one tick batch into one update() at the flush.
    sample.set_count(4);
    sample.set_count(5);
    check(wakes.count == 2, "further setters in the same tick do not wake again");
    const ui::UpdateFlushStats stats = scheduler.flush();
    check(stats.updated == 1, "several setters produce one update()");
    check(label_text(root) == "Count: 5", "the flush applied the latest state");

    // An async continuation sets state from a completion; the next flush repaints it.
    tessera::runtime::CompletionQueue completions;
    completions.post(tessera::runtime::CompletionPriority::Visible,
                     [&sample]() { sample.set_count(9); });
    check(label_text(root) == "Count: 5", "nothing changes before the completion runs");
    completions.run(8);
    check(label_text(root) == "Count: 5", "the completion only scheduled the update");
    scheduler.flush();
    check(label_text(root) == "Count: 9",
          "an async state change repaints on the next frame without a manual update()");

    // An animated value moves toward its target over scheduler ticks instead of jumping. Click
    // starts a 300 ms ease-in-out tween from 1.0 to 0.5.
    {
        const int wakes_before = wakes.count;
        sample.handler_1(tessera::ui::ClickEvent{});
        check(sample.anim_opacity_ == 1.0, "an animated set does not jump");
        check(scheduler.pending() && wakes.count > wakes_before,
              "an active tween schedules the component and wakes the loop");
        scheduler.flush();
        check(scheduler.tick(0.15), "the tween is still moving halfway");
        check(sample.anim_opacity_ < 1.0 && sample.anim_opacity_ > 0.5,
              "the sampled value is between start and target");
        check(scheduler.pending(), "a moved value schedules an update for the same frame");
        scheduler.flush();
        check(!scheduler.tick(0.2), "the tween finishes");
        check(sample.anim_opacity_ == 0.5, "the sampled value lands on the target");
        scheduler.flush();
        check(!scheduler.tick(0.016) && !scheduler.pending(), "an idle component costs no frame");
        // `animate: false` snaps and never schedules a tween.
        sample.handler_2(tessera::ui::TextChangeEvent{});
        check(sample.anim_opacity_ == 1.0, "animate:false snaps to the value");
        check(!sample.anim_opacity_scalar_.active(), "a snap leaves no active tween");
        scheduler.flush();
        sample.set_count(9);
        scheduler.flush();
    }

    // Detached components keep the manual contract: schedule_update is a harmless no-op.
    sample.detach_scheduler(scheduler);
    sample.set_count(11);
    check(!scheduler.pending(), "a detached component does not schedule");
    check(label_text(root) == "Count: 9", "a detached setter still needs a manual update()");
    sample.update();
    check(label_text(root) == "Count: 11", "update() remains public and works");

    if (failures == 0) {
        std::cout << "codegen_schedule_runtime: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
