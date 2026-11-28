// SPDX-License-Identifier: MIT
//
// The router shim's service (docs/specs/router-shim-v1.md, issue #549): what `navigate(...)` of a
// ported react-router component does once it has lowered to `RouterService`. Route match, params,
// back, a refused path and a deep link are asserted through the service over the real
// `NavController`, so the shim promises nothing the controller does not already do.

#include "appfw_navigation_test_support.hpp"
#include "tessera/appfw/router_service.hpp"

namespace nav_test {
namespace {

using tessera::appfw::NavRouterService;
using tessera::appfw::RouterService;

void push_matches_a_route_and_carries_its_params() {
    Harness harness;
    NavRouterService router(harness.controller);
    RouterService& service = router;
    service.push("/orders/42");
    expect(service.last_error() == NavigationErrorCode::None, "a known path is accepted");
    const NavEntry* active = harness.controller.active();
    expect(active != nullptr && active->route_id == "orders.detail", "the route matched");
    const auto* id = active == nullptr ? nullptr : tessera::appfw::find_param(active->params, "id");
    expect(id != nullptr && *id == "42", "the :id param reached the entry");
}

void a_refused_path_is_recorded_and_changes_nothing() {
    Harness harness;
    NavRouterService router(harness.controller);
    router.push("/orders");
    const std::size_t depth = harness.controller.depth();
    router.push("/orders/not-a-number");
    expect(router.last_error() != NavigationErrorCode::None, "an invalid param is refused");
    expect(harness.controller.depth() == depth, "the stack is untouched by a refusal");
    router.push("/settings");
    expect(router.last_error() == NavigationErrorCode::None, "the next accepted request clears it");
}

void replace_does_not_grow_history() {
    Harness harness;
    NavRouterService router(harness.controller);
    router.push("/orders");
    router.replace("/settings");
    expect(harness.controller.depth() == 1, "replace keeps the depth");
    expect(harness.controller.active()->route_id == "settings", "and swaps the active route");
}

void back_is_navigate_minus_one() {
    Harness harness;
    NavRouterService router(harness.controller);
    router.push("/orders");
    router.push("/settings");
    router.back();
    expect(harness.controller.active()->route_id == "orders", "back pops to the previous entry");
    router.back();
    router.back();
    expect(router.last_error() == NavigationErrorCode::None, "back at the root is not an error");
}

void a_deep_link_lands_on_the_stack_the_service_navigates() {
    Harness harness;
    const auto installed = install_deep_link(harness.table, harness.controller, link_config(),
                                             "myapp://orders/7", ActivationSource::Url, true);
    expect(installed.has_value(), "the link installs");
    NavRouterService router(harness.controller);
    router.back();
    expect(harness.controller.active()->route_id == "home",
           "the service's back reaches the synthesized root");
    router.push("/orders/9");
    expect(harness.controller.active()->route_id == "orders.detail", "and navigates on from there");
}

} // namespace
} // namespace nav_test

int main() {
    nav_test::push_matches_a_route_and_carries_its_params();
    nav_test::a_refused_path_is_recorded_and_changes_nothing();
    nav_test::replace_does_not_grow_history();
    nav_test::back_is_navigate_minus_one();
    nav_test::a_deep_link_lands_on_the_stack_the_service_navigates();
    return nav_test::g_failures;
}
