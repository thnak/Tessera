// SPDX-License-Identifier: MIT

// End-to-end proof of generated `onSizeChange` (ADR-041 slice 5): the compiled Watcher/WatchPage
// components deliver the observed size to an inline `e.width >= 600` handler and to a native
// handler, through the real LayoutObserverSet and settle loop. A rebuild after attach re-attaches,
// and detach removes every observation. No GPU.

#include "WatchPage.hpp"
#include "Watcher.hpp"
#include "tessera/ui/layout/layout_observer_settle.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <iostream>
#include <string_view>

namespace {

namespace ui = tessera::ui;
namespace layout = tessera::ui::layout;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// root(1000) -> view(401) -> { watcher host(402) -> view(301) -> text(302), tail text(403) }. The
// instance's own nodes carry its `node_id_base_` (issue #295), so its ids are 301 and 302 plus it.
void publish(layout::LayoutReadModel& model, float watcher_width, ui::NodeId instance_base) {
    layout::LayoutResult result{{layout::LayoutBox{1000, {0, 0, 800, 600}},
                                 layout::LayoutBox{401, {0, 0, 800, 600}},
                                 layout::LayoutBox{402, {0, 0, 800, 100}},
                                 layout::LayoutBox{301 + instance_base, {0, 0, watcher_width, 40}},
                                 layout::LayoutBox{302 + instance_base, {0, 0, 50, 20}},
                                 layout::LayoutBox{403, {0, 0, 120, 20}}}};
    model.publish(result, 1.0f);
}

void test_delivery_and_detach() {
    ui::Node root(1000);
    demo::gen::WatchPage page;
    page.build(root);

    ui::UpdateScheduler scheduler;
    page.attach_scheduler(scheduler);
    layout::LayoutObserverSet observers;
    page.attach_layout_observers(observers);
    check(observers.size() == 3, "three observed nodes attach: the instance's two and the tail");

    layout::LayoutReadModel model;
    layout::LayoutSettleState settle;
    publish(model, 700.0f, page.child_2_.node_id_base_);
    layout::observe_and_settle(observers, model, scheduler, layout::SettleHooks{}, settle);
    check(page.child_2_.state_wide_, "the inline handler saw width 700 and set wide");
    check(page.child_2_.state_resized_ == 1, "the parameterless handler ran");

    publish(model, 500.0f, page.child_2_.node_id_base_);
    layout::observe_and_settle(observers, model, scheduler, layout::SettleHooks{}, settle);
    check(!page.child_2_.state_wide_, "a later, narrower size clears wide");

    page.detach_layout_observers(observers);
    check(observers.size() == 0, "detach removes every observation, instance included");
    publish(model, 900.0f, page.child_2_.node_id_base_);
    layout::observe_and_settle(observers, model, scheduler, layout::SettleHooks{}, settle);
    check(!page.child_2_.state_wide_, "a detached component receives nothing");
    page.detach_scheduler(scheduler);
}

void test_build_after_attach() {
    ui::Node root(1000);
    demo::gen::WatchPage page;
    layout::LayoutObserverSet observers;
    page.attach_layout_observers(observers);
    check(observers.size() == 0, "nothing to attach before build");
    page.build(root);
    check(observers.size() == 3, "build() attaches to the set the component already joined");
    page.detach_layout_observers(observers);
    check(observers.size() == 0, "detach after a late build removes them");
}

} // namespace

int main() {
    test_delivery_and_detach();
    test_build_after_attach();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
