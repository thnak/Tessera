// SPDX-License-Identifier: MIT
//
// Acceptance word 3: **deep-link** (docs/specs/navigation-v1.md section 5; docs/specs/
// deep-linking-v1.md sections 2, 3, 5, 7 and 8; issue #196).
//
// An external URL/route arriving cold (no stack) must synthesize a sensible stack. Also: the warm
// case, the allowlists, and the malformed/unknown inputs, which are values and never crashes.

#include "appfw_navigation_test_support.hpp"

#include <string>
#include <utility>

namespace nav_test {
namespace {

void a_cold_deep_link_synthesizes_a_stack_with_a_root_below_it() {
    // The acceptance case: no stack at all, then a link. Section 5: "Cold start: wait for
    // app/route/auth readiness; preserve the pending link". The synthesis is what makes back
    // meaningful afterwards - the target is not a root, so `back()` reaches the home route instead
    // of reporting `AtRoot` immediately.
    Harness harness;
    expect(harness.controller.depth() == 0, "the app starts with no stack");

    const auto installed = install_deep_link(harness.table, harness.controller, link_config(),
                                             "myapp://orders/42", ActivationSource::Url, true);
    expect(installed.has_value(), "a cold deep link installs");
    expect(installed->outcome == DeepLinkOutcome::Synthesized, "the outcome is Synthesized");
    expect(installed->route_id == "orders.detail", "the route id is the resolved canonical id");
    expect(harness.controller.depth() == 2, "the root and the target are both on the stack");
    expect(harness.controller.stack()[0].route_id == "home", "the root is below the target");
    expect(harness.controller.active()->route_id == "orders.detail", "the target is active");
    expect(tessera::appfw::find_param(harness.controller.active()->params, "id") != nullptr,
           "the link's param survived into the entry");

    // And back from a cold deep link lands on home, then reports the root edge. That is the
    // "sensible stack" the acceptance asks for: without the synthesis the first back would already
    // be AtRoot.
    expect(harness.controller.back().outcome == BackOutcome::Popped,
           "back pops the deep-linked target");
    expect(harness.controller.active()->route_id == "home", "and lands on the route root");
    expect(harness.controller.back().outcome == BackOutcome::AtRoot,
           "the next back is the root edge");
}

void a_link_to_the_root_does_not_duplicate_it() {
    // The synthesis is skipped when the target *is* the root, because pushing the root under itself
    // would make back-at-root a no-op twice over.
    Harness harness;
    const auto installed = install_deep_link(harness.table, harness.controller, link_config(),
                                             "myapp://", ActivationSource::Url, true);
    expect(installed.has_value(), "a link to the root installs");
    expect(installed->route_id == "home", "and resolves to the root route");
    expect(harness.controller.depth() == 1, "the root is on the stack exactly once");
    expect(harness.controller.back().outcome == BackOutcome::AtRoot, "and back is the root edge");
}

void a_warm_deep_link_pushes_onto_the_existing_stack() {
    // Section 2 of deep-linking-v1: "Warm start (the app is already running): route immediately."
    Harness harness;
    (void)harness.controller.push("/settings");
    const auto installed = install_deep_link(harness.table, harness.controller, link_config(),
                                             "myapp://orders/42", ActivationSource::Url, false);
    expect(installed.has_value(), "a warm deep link installs");
    expect(installed->outcome == DeepLinkOutcome::Pushed, "the outcome is Pushed, not Synthesized");
    expect(harness.controller.depth() == 2, "the existing stack grew by one");
    expect(harness.controller.stack()[0].route_id == "settings", "the existing entry is untouched");
    expect(harness.controller.active()->route_id == "orders.detail", "the target is active");

    // A duplicate of the active route obeys the per-route policy: the stack does not grow twice.
    const auto duplicate = install_deep_link(harness.table, harness.controller, link_config(),
                                             "myapp://orders/42", ActivationSource::Url, false);
    expect(duplicate.has_value(), "a duplicate link is a success");
    expect(duplicate->outcome == DeepLinkOutcome::DuplicateIgnored, "reported as DuplicateIgnored");
    expect(harness.controller.depth() == 2, "and the stack did not grow");
}

void an_https_link_is_checked_against_both_allowlists() {
    Harness harness;
    // deep-linking-v1.md section 3: "schemes/hosts are allowlists; no wildcard hosts."
    expect(!parse_deep_link("other://orders", link_config()).has_value(),
           "an undeclared scheme is refused");
    expect(parse_deep_link("other://orders", link_config()).error().code ==
               NavigationErrorCode::UnsupportedScheme,
           "with UnsupportedScheme");

    expect(!parse_deep_link("https://evil.example.com/orders", link_config()).has_value(),
           "an undeclared HTTPS host is refused");
    expect(parse_deep_link("https://evil.example.com/orders", link_config()).error().code ==
               NavigationErrorCode::UnsupportedHost,
           "with UnsupportedHost");

    // The declared host is accepted, and its path resolves exactly as the custom scheme's does.
    const auto https =
        install_deep_link(harness.table, harness.controller, link_config(),
                          "https://app.example.com/orders/7", ActivationSource::Url, true);
    expect(https.has_value(), "the declared host installs");
    expect(https->route_id == "orders.detail",
           "and resolves to the same route as the custom scheme");

    // A custom-scheme URL parses its first segment as the authority (`myapp://orders/42`). The host
    // allowlist therefore must not apply to it, or every custom-scheme link would be refused.
    const auto custom = parse_deep_link("myapp://orders/42", link_config());
    expect(custom.has_value(), "a custom-scheme link is not checked as an HTTPS host");
    expect(custom->path == "/orders/42", "and its path starts at the first segment");
}

void malformed_and_unknown_links_are_named_failures() {
    struct Case {
        std::string url;
        NavigationErrorCode code;
    };
    const Case cases[] = {
        // Section 7: "Max URL length; reject malformed/oversized/percent-encoded traversal/control
        // chars."
        {"myapp://orders/" + std::string(5000, '1'), NavigationErrorCode::PathTooLong},
        {"myapp://orders/%2f", NavigationErrorCode::EncodedSeparator},
        {"myapp://orders/%zz", NavigationErrorCode::MalformedPercentEncoding},
        {"myapp://orders/%00", NavigationErrorCode::ControlCharacter},
        {"myapp://orders/\xC3\x28", NavigationErrorCode::InvalidUtf8},
        // A link with no path at all is the root, not malformed; a link that is empty is malformed.
        {"", NavigationErrorCode::InvalidPath},
    };
    for (const Case& test : cases) {
        const auto parsed = parse_deep_link(test.url, link_config());
        expect(!parsed.has_value(), "a malformed link does not parse");
        expect(parsed.error().code == test.code, "the failure names the malformed class");
    }

    // A bare path is refused unless the config says the source supplies one (a command line or a
    // harness).
    expect(!parse_deep_link("/orders/42", link_config(false)).has_value(),
           "a bare path is refused by default");
    expect(parse_deep_link("/orders/42", link_config(true)).has_value(),
           "and accepted when the source declares it");

    // A well-formed link to a route the table does not have is `NotFound`, per section 5's "never
    // silently ignore", and it does not touch the stack.
    Harness harness;
    (void)harness.controller.push("/settings");
    const auto unknown = install_deep_link(harness.table, harness.controller, link_config(),
                                           "myapp://no/such/screen", ActivationSource::Url, false);
    expect(!unknown.has_value(), "an unknown route does not install");
    expect(unknown.error().code == NavigationErrorCode::NotFound, "with NotFound");
    expect(harness.controller.depth() == 1, "and the stack is unchanged");

    // Section 8: "Unknown/invalid route: error/home route; diagnostic; no crash." The not-found
    // route is reachable through the same commit path, so the caller can route the failure there.
    // `not_found_route()` is a route *id*, which `push_route` takes; `push` takes a path.
    expect(harness.controller.push_route(harness.table.not_found_route()).has_value(),
           "the not-found route itself is navigable");
    // The pointer is read only after the `expect` above succeeded; a failed push would have left
    // the stack at its previous entry, which is why this is guarded rather than dereferenced blind.
    const NavEntry* active = harness.controller.active();
    expect(active != nullptr && active->route_id == "not_found",
           "and it is what the caller routes to");
}

void a_typed_param_failure_refuses_the_link() {
    Harness harness;
    // `id` is an `Int`; a link that supplies text is refused where a route match would be, so the
    // failure names the param and the stack is untouched.
    const auto bad = install_deep_link(harness.table, harness.controller, link_config(),
                                       "myapp://orders/not-a-number", ActivationSource::Url, true);
    expect(!bad.has_value(), "a link with a bad typed param does not install");
    expect(bad.error().code == NavigationErrorCode::ParamInvalid, "with ParamInvalid");
    expect(bad.error().subject == "id", "and it names the param");
    expect(harness.controller.depth() == 0, "the stack is untouched");
}

void an_auth_gated_target_is_reported_not_silently_mounted() {
    // navigation-v1.md section 4: "Auth-gated routes wait for auth state; deep links preserve the
    // intended destination", and section 10: "Auth guards are enforced before mounting protected
    // screens/data." This slice has no guard pipeline, so it reports the flag for the host's own
    // gate rather than mounting a protected screen or inventing a pending-queue policy the specs
    // leave open.
    Harness harness;
    const auto installed = install_deep_link(harness.table, harness.controller, link_config(),
                                             "myapp://orders/42", ActivationSource::Url, true);
    expect(installed.has_value(), "the link installs so the host can ask");
    expect(installed->requires_auth, "and the result says the target is auth-gated");

    // A route with `auth = false` reports false, so the host can skip its gate.
    (void)harness.controller.push("/settings");
    const auto open = install_deep_link(harness.table, harness.controller, link_config(),
                                        "myapp://settings", ActivationSource::Url, false);
    expect(open.has_value(), "an open route installs");
    expect(!open->requires_auth, "and does not report a gate");
}

void a_link_is_normalized_before_a_diagnostic_sees_it() {
    // The normalized form is what a dedupe key and a diagnostic log read: scheme and host
    // lowercased, the fragment dropped, the query kept. A link's params are untrusted, so the log
    // entry is the URL the parser accepted rather than the raw platform intent.
    const auto parsed = parse_deep_link("HTTPS://APP.EXAMPLE.COM/orders/42?tab=items#frag",
                                        link_config(), ActivationSource::Url, true);
    expect(parsed.has_value(), "a mixed-case App/Universal Link parses");
    expect(parsed->url == "https://app.example.com/orders/42?tab=items",
           "scheme and host are lowercased and the fragment is dropped");
    // `path` is the match input, so it carries no query: `RouteMatch::query` is the separate,
    // untrusted map (navigation-v1.md section 3's "query is not a substitute for typed params").
    expect(parsed->path == "/orders/42", "the path is the match input and excludes the query");
    expect(parsed->source == ActivationSource::Url, "the source is recorded");
    expect(parsed->cold_start, "the cold-start flag is recorded");

    // A custom scheme's first segment is a route segment, so its normalization only lowercases the
    // scheme and keeps the rest as written.
    const auto custom = parse_deep_link("MyApp://orders/42", link_config());
    expect(custom.has_value(), "a mixed-case custom scheme parses");
    expect(custom->url == "myapp://orders/42", "the scheme is lowercased and the path is kept");
    expect(custom->path == "/orders/42", "and the canonical path starts at the first segment");

    // A query value may contain an encoded `/`: it is data, not a re-segmentation risk, so it is
    // not rejected the way an encoded separator in a *path* segment is.
    const auto with_slash = parse_deep_link("myapp://settings?next=%2Forders%2F42", link_config());
    expect(with_slash.has_value(), "an encoded slash in a query value is data, not a separator");
}

void the_resolved_link_carries_the_typed_params() {
    const RouteTable table = make_table();
    auto parsed = parse_deep_link("myapp://orders/42", link_config(true));
    expect(parsed.has_value(), "the link parses");
    auto resolved = resolve_deep_link(table, std::move(*parsed));
    expect(resolved.has_value(), "the link resolves");
    expect(resolved->route_id == "orders.detail", "the canonical route id is recorded");
    const std::string* id = tessera::appfw::find_param(resolved->params, "id");
    expect(id != nullptr && *id == "42", "the typed param is recorded");
    expect(resolved->rest.empty(), "and a route without a wildcard has an empty rest");
}

} // namespace

int run_deep_link_tests() {
    a_cold_deep_link_synthesizes_a_stack_with_a_root_below_it();
    a_link_to_the_root_does_not_duplicate_it();
    a_warm_deep_link_pushes_onto_the_existing_stack();
    an_https_link_is_checked_against_both_allowlists();
    malformed_and_unknown_links_are_named_failures();
    a_typed_param_failure_refuses_the_link();
    an_auth_gated_target_is_reported_not_silently_mounted();
    a_link_is_normalized_before_a_diagnostic_sees_it();
    the_resolved_link_carries_the_typed_params();
    return g_failures;
}

} // namespace nav_test
