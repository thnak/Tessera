// SPDX-License-Identifier: MIT
//
// `useService<T>()` from TSX, end to end (ADR-050 section 3.1, issue #177). `Inbox.g.hpp` is
// generated at build time from `nav.push("/compose")` in a TSX click handler
// (codegen_services_tsx_emit), and this test mounts the component against a provided
// `demo::Navigator`, clicks, and checks the call reached it: the TSX said "push /compose", the
// Navigator saw "/compose". Resolution happens once, at mount; the click performs no registry
// lookup.

#include "Inbox.hpp"
#include "demo/services.hpp"
#include "tessera/appfw/service_context.hpp"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    demo::NavigatorImpl navigator;
    tessera::appfw::ServiceContext services;
    check(services.provide<demo::Navigator>(navigator).has_value(), "the Navigator is provided");
    check(services.freeze().has_value(), "the registry freezes");

    demo::Inbox inbox;
    tessera::ui::Node root(900);
    inbox.build(root);
    inbox.mount(services);
    check(navigator.pushes() == 0, "mounting does not call the service");

    tessera::appfw::reset_service_lookup_audit();
    {
        tessera::appfw::FrameScope frame;
        tessera::ui::Event click;
        click.type = tessera::ui::EventType::Click;
        check(!root.children().empty() && root.children()[0]->run_event_bindings(click) == 1,
              "the View has the generated click binding");
    }
    check(tessera::appfw::service_lookup_audit().in_frame == 0,
          "the handler's service call performs no registry lookup");
    check(navigator.pushes() == 1, "the click called Navigator::push once");
    check(navigator.last_route() == "/compose", "with the route the TSX wrote");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen services tsx runtime tests passed\n";
    return 0;
}
