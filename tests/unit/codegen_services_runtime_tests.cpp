// SPDX-License-Identifier: MIT

// ADR-050 section 3.1 (AF5, issue #177), runtime side: a developer class derives from the generated
// `InboxBase<Self>`, `mount(context)` resolves each declared requirement exactly once, and a
// `service<T>()` / `<name>()` read is a cached pointer with no registry traffic.
//
// This file is the resolve-cost GATE. A wall-clock number cannot be a gate (the machine moves, the
// compiler moves), so the cost is asserted the way ADR-050 section 3 item 5 defines it: AF1's
// `service_lookup_audit()` counts lookups that happen inside a `FrameScope`, which is exactly the
// render/handler window where ADR-050 forbids a resolve. The assertions are therefore:
//   - mount(context) resolves each requirement exactly once (one audit `missing` miss at most, and
//     no per-read growth), and
//   - N accessor reads in a frame add ZERO in-frame lookups,
//   - and every read returned the RIGHT service, so a counter of zero cannot be satisfied by an
//     accessor that does nothing.
// The wall-clock comparison behind these numbers is the documented measurement in
// docs/design/service-resolve-benchmark.md (tools/poc/service_resolve), which this test does not
// duplicate.

#include "Inbox.hpp"
#include "demo/services.hpp"
#include "tessera/appfw/service_context.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::appfw::FrameScope;
using tessera::appfw::reset_service_lookup_audit;
using tessera::appfw::service_lookup_audit;
using tessera::appfw::ServiceContext;

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
    demo::MailStoreImpl store(7);

    ServiceContext services;
    check(services.provide<demo::Navigator>(navigator).has_value(),
          "the composition root provides the Navigator contract");
    check(services.provide<demo::MailStore>(store).has_value(),
          "the composition root provides the MailStore contract");
    check(services.freeze().has_value(), "the composition root freezes the registry");

    demo::Inbox inbox;
    tessera::ui::Node root(900);

    // --- mount(context) resolves each requirement once -----------------------------------------
    reset_service_lookup_audit();
    inbox.build(root);
    inbox.mount(services);
    const auto after_mount = service_lookup_audit();
    check(after_mount.missing == 0, "a complete registry resolves every declared service");

    // --- a read is a cached pointer, not a lookup: ZERO in-frame lookups ----------------------
    // ADR-050 section 3 item 5 forbids a resolve while a frame is being updated. The generated
    // accessor never touches ServiceContext, so the counter stays flat however many times it is
    // read.
    reset_service_lookup_audit();
    {
        FrameScope frame;
        for (int i = 0; i < 1000; ++i) {
            demo::Navigator& resolved = inbox.nav();
            // The value check: a reader that returned garbage, or nothing at all, would not be
            // able to report this route back.
            resolved.push("page-" + std::to_string(i));
        }
    }
    const auto after_reads = service_lookup_audit();
    check(after_reads.in_frame == 0,
          "1000 accessor reads inside a frame perform no service lookup at all");
    check(after_reads.missing == 0, "and no read misses");
    check(navigator.pushes() == 1000, "every read reached the real provided Navigator");
    check(navigator.last_route() == "page-999", "the reads returned the right service instance");

    // The second requirement is a distinct object, resolved through its own slot: a tuple index
    // mix-up would show up here rather than in the counter.
    check(&inbox.store() == &store, "store() returns the MailStore the composition root provided");
    check(inbox.store().unread() == 7, "and reads through it see its state");

    // --- a registered service the component never asked for stays unreachable ------------------
    // `demo::Settings` is a valid service interface; the fixture component simply does not declare
    // it, which is what TESSERA_CG_SERVICE_UNDECLARED rejects at compile time. The compile-fail
    // target `codegen_base_service_undeclared` is that half of the acceptance.
    check(!inbox.has_service_v<demo::Settings>,
          "an undeclared requirement is a compile-time false, never a runtime miss");

    // --- the event handler path reads through the same cached pointers --------------------------
    reset_service_lookup_audit();
    {
        FrameScope frame;
        tessera::ui::Event click;
        click.type = tessera::ui::EventType::Click;
        check(root.children()[0]->run_event_bindings(click) == 1,
              "the Button has the generated click binding");
    }
    check(service_lookup_audit().in_frame == 0,
          "an event handler reading a service performs no lookup either");
    check(inbox.accessor_reads == 1, "the handler ran and read its accessor once");
    check(inbox.last_route == "inbox", "the handler's service call reached the provided Navigator");
    check(navigator.last_route() == "inbox", "and the Navigator saw it");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen services runtime tests passed\n";
    return 0;
}
