// SPDX-License-Identifier: MIT
//
// `useObservable(svc.member)` from TSX, end to end (ADR-050 open question 6). `Header.g.hpp` is
// generated at build time from a component that reads `nav.title` and `nav.unread` in render
// (codegen_observable_tsx_emit); this test mounts it against a provided `demo::Navigator` whose
// observables the test writes, and checks the properties the design note states: a read shows the
// live value, a `set()` re-renders the component once at the next flush however many times it was
// called, an unchanged value schedules nothing, a binding that reads another observable is not
// re-run, and an unmounted component is no longer notified.

#include "Header.hpp"
#include "demo/observable_nav.hpp"
#include "tessera/appfw/service_context.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string text_of(const tessera::ui::Node& view, std::size_t index) {
    const auto& children = view.children();
    if (index >= children.size() || !children[index]->paint_data().has_value()) {
        return "<none>";
    }
    return children[index]->paint_data()->text;
}

} // namespace

int main() {
    tessera::ui::UpdateScheduler scheduler;
    demo::NavigatorImpl navigator(scheduler);
    tessera::appfw::ServiceContext services;
    check(services.provide<demo::Navigator>(navigator).has_value(), "the Navigator is provided");
    check(services.freeze().has_value(), "the registry freezes");

    demo::Header header;
    tessera::ui::Node root(900);
    header.build(root);
    check(header.obs_title_value().empty(), "before attach the observable reads its zero value");
    header.attach_scheduler(scheduler);
    header.mount(services);
    header.update(); // the host's first render after mount
    const tessera::ui::Node& view = *root.children().front();
    check(text_of(view, 0) == "Inbox", "the first render shows the observable's initial value");
    check(text_of(view, 1) == "0 new", "an int observable renders through the template");
    check(navigator.title().observer_count() == 1 && navigator.unread().observer_count() == 1,
          "mount subscribed once per observable");

    // Many writes in one tick: one notification, one update().
    (void)scheduler.flush();
    for (int i = 1; i <= 50; ++i) {
        navigator.unread().set(i);
    }
    const auto stats = scheduler.flush();
    check(text_of(view, 1) == "50 new", "the render shows the last value written");
    check(stats.updated >= 2, "the observable's ticket and the component's ticket both ran");
    check(text_of(view, 0) == "Inbox", "the binding that reads the other observable is untouched");

    // A repeated identical write schedules nothing.
    navigator.unread().set(50);
    const auto idle = scheduler.flush();
    check(idle.updated == 0 && !scheduler.pending(), "an unchanged value runs no update");

    navigator.title().set("Drafts");
    (void)scheduler.flush();
    check(text_of(view, 0) == "Drafts", "a string observable updates its text");

    // Unmount releases the subscriptions: a later write does not reach the component.
    header.unmount();
    check(navigator.title().observer_count() == 0 && navigator.unread().observer_count() == 0,
          "unmount unsubscribed from both observables");
    navigator.title().set("Sent");
    (void)scheduler.flush();
    check(text_of(view, 0) == "Drafts", "an unmounted component is not updated");

    header.detach_scheduler(scheduler);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen observable tsx runtime tests passed\n";
    return 0;
}
