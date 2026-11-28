// SPDX-License-Identifier: MIT
//
// `LinkRouter` (issue #542, ADR-059 group B): an internal href goes through `NavController` and its
// route table and restores on back; an external URL reaches the (fake) shell service when its
// scheme is allowed; every refusal is a stable outcome and a counter, never a throw. Deterministic:
// the route table is data, the platform is `FakeShell`, and delivery is a hand-drained queue.

#include "appfw_navigation_test_support.hpp"
#include "appfw_shell_test_support.hpp"
#include "tessera/appfw/link_router.hpp"

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "appfw_link_router: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

using tessera::appfw::LinkOutcome;
using tessera::appfw::LinkPolicy;
using tessera::appfw::LinkRouter;

void internal_href_navigates_and_restores() {
    nav_test::Harness nav;
    shell_test::Harness shell;
    LinkRouter router(&nav.controller, &shell.service);

    expect(router.open("/orders").outcome == LinkOutcome::Navigated, "/orders navigates");
    expect(router.open("/orders/42").outcome == LinkOutcome::Navigated, "a param route navigates");
    expect(nav.controller.commit(), "the pushes publish");
    expect(nav.controller.active() != nullptr &&
               nav.controller.active()->route_id == "orders.detail",
           "the param route is active");
    expect(nav.controller.depth() == 2, "two entries");

    const tessera::appfw::BackResult back = nav.controller.back();
    expect(back.outcome == tessera::appfw::BackOutcome::Popped, "back pops the linked entry");
    expect(nav.controller.commit(), "the back publishes");
    expect(nav.controller.active() != nullptr && nav.controller.active()->route_id == "orders",
           "back restores the previous route");
    expect(shell.fake.received().empty(), "an internal link never reaches the shell");
    expect(router.diagnostics().navigated == 2, "two navigations counted");
}

void unresolved_internal_path_is_a_diagnostic() {
    nav_test::Harness nav;
    LinkRouter router(&nav.controller, nullptr);
    expect(router.open("/orders").ok(), "a valid path first");
    expect(nav.controller.commit(), "published");
    const std::size_t depth = nav.controller.depth();

    const auto result = router.open("/orders/not-an-int");
    expect(result.outcome == LinkOutcome::UnresolvedPath,
           "a param the table rejects is unresolved");
    expect(!result.detail.empty(), "the reason is named");
    expect(router.open("/no/such/route").outcome == LinkOutcome::UnresolvedPath,
           "an unknown path is unresolved");
    expect(nav.controller.depth() == depth, "a refused link leaves the stack as it was");
    expect(router.diagnostics().unresolved_path == 2, "both counted");
}

void external_url_reaches_the_shell() {
    nav_test::Harness nav;
    shell_test::Harness shell;
    LinkRouter router(&nav.controller, &shell.service);

    expect(router.open("https://example.com/a?b=1").outcome == LinkOutcome::OpenedExternal,
           "https opens");
    expect(router.open("MAILTO:team@example.com").outcome == LinkOutcome::OpenedExternal,
           "the scheme is case-insensitive");
    shell_test::pump(shell);
    expect(shell.fake.received().size() == 2, "both reached the fake shell");
    expect(shell.fake.received()[0].kind == tessera::appfw::FakeShellCall::Kind::OpenUrl,
           "as OpenUrl calls");
    expect(shell.fake.received()[0].text == "https://example.com/a?b=1",
           "the URL is passed verbatim");
    expect(nav.controller.depth() == 0, "an external link never touches navigation");
    expect(router.diagnostics().opened_external == 2, "two counted");
    expect(router.diagnostics().external_failed == 0, "none failed");
}

void platform_failure_is_counted() {
    shell_test::Harness shell;
    shell.fake.set_open_url_ok(false);
    LinkRouter router(nullptr, &shell.service);
    expect(router.open("https://example.com").outcome == LinkOutcome::OpenedExternal,
           "the hand-off was accepted");
    shell_test::pump(shell);
    expect(router.diagnostics().external_failed == 1, "the platform failure is counted");
}

void schemes_outside_the_policy_are_refused() {
    shell_test::Harness shell;
    LinkRouter router(nullptr, &shell.service);
    for (const char* href : {"javascript:alert(1)", "file:///etc/passwd", "data:text/html,hi",
                             "ftp://example.com", "intent://x#Intent;end"}) {
        const auto result = router.open(href);
        expect(result.outcome == LinkOutcome::SchemeNotOpened, "a scheme off the list is refused");
        expect(result.detail.find(href) == std::string::npos, "the detail never echoes the URL");
    }
    shell_test::pump(shell);
    expect(shell.fake.received().empty(), "nothing reached the shell");
    expect(router.diagnostics().scheme_not_opened == 5, "five refusals counted");

    LinkPolicy https_only;
    https_only.external_schemes = {"https"};
    LinkRouter strict(nullptr, &shell.service, https_only);
    expect(strict.open("http://example.com").outcome == LinkOutcome::SchemeNotOpened,
           "an app can drop http");
    expect(strict.open("https://example.com").outcome == LinkOutcome::OpenedExternal,
           "and keep https");
}

void malformed_and_unavailable() {
    shell_test::Harness shell;
    LinkRouter router(nullptr, &shell.service);
    for (const char* href : {"", "//evil.example/x", "relative/path", "#fragment", "1http://x"}) {
        expect(router.open(href).outcome == LinkOutcome::Malformed, "malformed href");
    }
    expect(router.open(std::string(3000, 'a').insert(0, "https://")).outcome ==
               LinkOutcome::Malformed,
           "an oversize URL is malformed");
    expect(router.open("/orders").outcome == LinkOutcome::ServiceUnavailable,
           "an internal link with no navigation controller");

    LinkRouter no_shell(nullptr, nullptr);
    expect(no_shell.open("https://example.com").outcome == LinkOutcome::ServiceUnavailable,
           "an external link with no shell");

    tessera::appfw::ShellCapabilities none;
    shell_test::Harness denied(tessera::appfw::ShellConfig{}, none);
    LinkRouter without_capability(nullptr, &denied.service);
    expect(without_capability.open("https://example.com").outcome ==
               LinkOutcome::ServiceUnavailable,
           "a shell without the OpenUrl capability");
    expect(denied.fake.received().empty(), "nothing started");
}

} // namespace

int main() {
    internal_href_navigates_and_restores();
    unresolved_internal_path_is_a_diagnostic();
    external_url_reaches_the_shell();
    platform_failure_is_counted();
    schemes_outside_the_policy_are_refused();
    malformed_and_unavailable();
    if (g_failures != 0 || nav_test::g_failures != 0 || shell_test::g_failures != 0) {
        std::cerr << "appfw_link_router: failures\n";
        return 1;
    }
    std::cout << "appfw_link_router: ok\n";
    return 0;
}
