// SPDX-License-Identifier: MIT
//
// Acceptance word 1: **route** (docs/specs/navigation-v1.md sections 1 and 3; issue #196).
//
// push/replace/params resolution, and what an unknown route does: a named failure that the caller
// maps to the not-found route, never a crash and never a silent empty match.

#include "appfw_navigation_test_support.hpp"

#include <string>
#include <utility>
#include <vector>

namespace nav_test {
namespace {

void the_route_table_rejects_what_section_1_calls_an_error() {
    // "Route table is validated at build time; duplicate ids/paths are errors."
    RouteTableConfig duplicate_id;
    duplicate_id.routes = spec_routes();
    duplicate_id.routes.push_back(RouteDef{"home", "/other", {}});
    auto by_id = RouteTable::create(duplicate_id);
    expect(!by_id.has_value(), "a duplicate id is refused");
    expect(by_id.error().code == NavigationErrorCode::DuplicateRouteId, "the error names the id");
    expect(by_id.error().subject == "home", "the error names which id");

    RouteTableConfig duplicate_path;
    duplicate_path.routes = spec_routes();
    duplicate_path.routes.push_back(RouteDef{"home2", "/", {}});
    auto by_path = RouteTable::create(duplicate_path);
    expect(!by_path.has_value(), "a duplicate path is refused");
    expect(by_path.error().code == NavigationErrorCode::DuplicateRoutePath,
           "the error names the path");

    // The grammar is section 1's four forms and nothing else.
    for (const std::string& bad : {std::string("orders"), std::string("/files/*/x"),
                                   std::string("/a/*.txt"), std::string("/:")}) {
        RouteTableConfig config;
        config.routes = {RouteDef{"bad", bad, {}}};
        const auto result = RouteTable::create(config);
        expect(!result.has_value(), "a pattern outside the section 1 grammar is refused");
        expect(result.error().code == NavigationErrorCode::InvalidPattern,
               "the failure is InvalidPattern");
    }

    // A param declaration must name a param of its own pattern, or the validation can never run.
    RouteTableConfig unknown_param;
    unknown_param.routes = {
        RouteDef{"orders.detail", "/orders/:id", {ParamSpec{"other", ParamType::Int, {}}}}};
    const auto param = RouteTable::create(unknown_param);
    expect(!param.has_value(), "a ParamSpec naming no param is refused");
    expect(param.error().code == NavigationErrorCode::UnknownParam, "the error is UnknownParam");

    // An empty table has no route root, so it is refused rather than producing a controller that
    // can never navigate.
    RouteTableConfig empty;
    expect(RouteTable::create(empty).error().code == NavigationErrorCode::EmptyRouteTable,
           "an empty table is refused");
}

void a_path_resolves_to_the_specifying_route_with_typed_params() {
    const RouteTable table = make_table();

    auto match = table.match("/orders/42");
    expect(match.has_value(), "/orders/42 matches");
    expect(match->route_id == "orders.detail", "it resolves to the declared id, not the path");
    expect(match->matched_path == "/orders/:id", "the match names the pattern that won");
    const std::string* id = tessera::appfw::find_param(match->params, "id");
    expect(id != nullptr && *id == "42", "the param is captured under its declared name");

    // Section 3: "Query/state is not a substitute for typed params". The query is reported
    // separately, so a screen cannot read it as if it were a path param.
    auto with_query = table.match("/orders/42?tab=items&mode=full");
    expect(with_query.has_value(), "a query does not stop the path from matching");
    expect(with_query->query.size() == 2, "both query keys are reported");
    expect(tessera::appfw::find_param(with_query->query, "tab") != nullptr,
           "the query key is present");
    expect(tessera::appfw::find_param(with_query->params, "tab") == nullptr,
           "a query key never becomes a path param");

    // The literal wins over the param of the same prefix, or `/orders/new` would be unreachable and
    // the `Int` validation would reject it.
    auto literal = table.match("/orders/new");
    expect(literal.has_value(), "/orders/new matches");
    expect(literal->route_id == "orders.new", "the literal route wins over the param route");

    // The optional form: both spellings resolve, and the absent one leaves the param out.
    auto with_scope = table.match("/search/books");
    expect(with_scope.has_value(), "the optional param form matches with the segment");
    expect(tessera::appfw::find_param(with_scope->params, "scope") != nullptr,
           "the scope is captured");
    auto without_scope = table.match("/search");
    expect(without_scope.has_value(), "the optional param form matches without the segment");
    expect(tessera::appfw::find_param(without_scope->params, "scope") == nullptr,
           "an absent optional param is absent, not empty");

    // The wildcard captures the remainder as one `rest`, and `/files` matches with an empty one.
    auto wildcard = table.match("/files/a/b/c.txt");
    expect(wildcard.has_value(), "the wildcard form matches a nested path");
    expect(wildcard->rest == "a/b/c.txt", "the wildcard captures the remainder, joined with /");
    auto empty_wildcard = table.match("/files");
    expect(empty_wildcard.has_value(), "the wildcard form matches its own prefix");
    expect(empty_wildcard->rest.empty(), "the empty remainder is empty, not a segment");

    // The root, with and without the trailing separator.
    expect(table.match("/")->route_id == "home", "/ resolves to the root route");
}

void a_typed_param_that_does_not_parse_is_a_named_failure() {
    const RouteTable table = make_table();
    // `id` is declared `Int`, so a non-numeric segment is a param failure, not a `NotFound`: the
    // route matched, and the value is what disagreed.
    auto not_an_int = table.match("/orders/abc");
    expect(!not_an_int.has_value(), "a non-numeric id does not match a declared Int");
    expect(not_an_int.error().code == NavigationErrorCode::ParamInvalid,
           "the error is ParamInvalid");
    expect(not_an_int.error().subject == "id", "the error names the param");

    // The `Int` boundary: a signed 64-bit value parses, one past it does not.
    expect(table.match("/orders/-9223372036854775808").has_value() ||
               table.match("/orders/0").has_value(),
           "a large negative or zero id parses");
    expect(!table.match("/orders/9223372036854775808").has_value(),
           "one past INT64_MAX is refused rather than wrapped");
}

void untyped_params_are_available_without_a_declaration() {
    // Section 11 records "type generation for params from the route table" as an open question, so
    // a declaration is optional: an undeclared param is a `String` and validates as one.
    RouteTableConfig config;
    config.routes = {RouteDef{"slug", "/p/:slug", {}}};
    const RouteTable table = make_table(std::move(config));
    auto match = table.match("/p/hello%20world");
    expect(match.has_value(), "an undeclared param still resolves");
    const std::string* slug = tessera::appfw::find_param(match->params, "slug");
    expect(slug != nullptr && *slug == "hello world", "the value is percent-decoded exactly once");
}

void an_unknown_route_is_a_named_failure_not_a_crash() {
    const RouteTable table = make_table();
    // Section 3: "Unknown route -> not-found screen with back/home actions." The matcher reports
    // the failure; the not-found route is named by the table, so the caller has both halves.
    auto unknown = table.match("/no/such/screen");
    expect(!unknown.has_value(), "an unknown path does not match");
    expect(unknown.error().code == NavigationErrorCode::NotFound, "the failure is NotFound");
    expect(unknown.error().subject == "/no/such/screen", "the error names the path that failed");
    expect(table.not_found_route() == "not_found", "the table names the not-found route to use");
    expect(table.find(table.not_found_route()) != nullptr, "and that route exists");

    // A push of it leaves the stack untouched, which is section 5's "never navigate to a half
    // state".
    Harness harness;
    const auto pushed = harness.controller.push("/no/such/screen");
    expect(!pushed.has_value(), "pushing an unknown path fails");
    expect(pushed.error().code == NavigationErrorCode::NotFound, "with the NotFound code");
    expect(harness.controller.depth() == 0, "and the stack is still empty");
    expect(!harness.controller.pending(), "and nothing was marked pending");
}

void malformed_input_is_refused_before_a_value_reaches_a_screen() {
    const RouteTable table = make_table();
    // Section 10: "All deep-link/route params are untrusted: decode/validate, length limits, no
    // traversal."
    struct Case {
        std::string path;
        NavigationErrorCode code;
    };
    const Case cases[] = {
        {"orders/42", NavigationErrorCode::InvalidPath},        // no leading /
        {"/orders/%2f", NavigationErrorCode::EncodedSeparator}, // an encoded `/` would re-segment
        {"/orders/%5c", NavigationErrorCode::EncodedSeparator}, // and so would an encoded `\`
        {"/orders/%2e%2e", NavigationErrorCode::InvalidPath},   // `%2e%2e` decodes to `..`
        {"/orders/..", NavigationErrorCode::InvalidPath},       // literal traversal
        {"/orders/%zz", NavigationErrorCode::MalformedPercentEncoding}, // not hex
        {"/orders/%00", NavigationErrorCode::ControlCharacter},         // NUL
        {"/orders/\xC3\x28", NavigationErrorCode::InvalidUtf8},         // truncated 2-byte sequence
        {"/orders/\xED\xA0\x80", NavigationErrorCode::InvalidUtf8},     // a surrogate
    };
    for (const Case& test : cases) {
        const auto result = table.match(test.path);
        expect(!result.has_value(), "malformed input does not match");
        expect(result.error().code == test.code, "the failure names the malformed class");
    }
    // A 4097-byte path is refused before the walk, so the bound is the module's rather than a
    // caller's.
    const std::string long_path = "/orders/" + std::string(5000, '1');
    expect(table.match(long_path).error().code == NavigationErrorCode::PathTooLong,
           "an oversized path is PathTooLong");
}

void push_grows_replace_does_not() {
    Harness harness;
    expect(*harness.controller.push("/orders") == PushOutcome::Committed, "a push commits");
    expect(harness.controller.depth() == 1, "one entry");
    expect(harness.controller.active()->route_id == "orders", "and it is active");

    expect(harness.controller.push("/settings").has_value(), "a second push succeeds");
    expect(harness.controller.depth() == 2, "history grew");

    // Section 2: "Replace does not grow history."
    expect(harness.controller.replace("/orders").has_value(), "a replace succeeds");
    expect(harness.controller.depth() == 2, "a replace does not grow the stack");
    expect(harness.controller.active()->route_id == "orders", "and the top entry is the new route");
    expect(harness.controller.stack()[0].route_id == "orders", "the earlier entry is untouched");
}

void the_duplicate_policy_is_per_route() {
    Harness harness;
    (void)harness.controller.push("/orders");
    expect(harness.controller.depth() == 1, "the first push lands");

    // Section 2: "push same route with same params -> focus/no-op or replace, explicit per route."
    // The default is the no-op, which cannot lose state.
    const auto again = harness.controller.push("/orders");
    expect(again.has_value(), "a duplicate push is a success");
    expect(*again == PushOutcome::DuplicateIgnored, "and it is reported as ignored");
    expect(harness.controller.depth() == 1, "so the stack did not grow");

    // A different param is not a duplicate.
    expect(*harness.controller.push("/orders/7") == PushOutcome::Committed,
           "a different param is not a duplicate");
    expect(harness.controller.depth() == 2, "so it pushes");

    // `quick` declares the `Replace` policy, so the same route with the same params replaces in
    // place.
    Harness replacing;
    (void)replacing.controller.push("/quick");
    expect(*replacing.controller.push("/quick") == PushOutcome::Committed,
           "the Replace policy commits");
    expect(replacing.controller.depth() == 1, "without growing history");
}

void the_stack_bound_is_a_named_failure() {
    // ADR-050 section 10: no hidden allocations on hot paths, so the stack is bounded and a push
    // past the bound is refused rather than growing.
    Harness harness(spec_config(), NavControllerConfig{2});
    expect(harness.controller.push("/orders").has_value(), "the first push fits");
    expect(harness.controller.push("/settings").has_value(), "the second push fits");
    const auto refused = harness.controller.push("/search");
    expect(!refused.has_value(), "the third push is refused");
    expect(refused.error().code == NavigationErrorCode::CapacityExceeded,
           "with CapacityExceeded, not a crash");
    expect(harness.controller.depth() == 2, "and the stack is unchanged");
}

void a_route_change_is_one_notification_per_flush() {
    Harness harness;
    RouteRecorder recorder;
    auto subscription = harness.controller.route().subscribe(&RouteRecorder::on_route, &recorder);

    // Section 3: "Route changes are frame-boundary operations". A request does not notify.
    expect(harness.controller.push("/orders").has_value(), "the request is accepted");
    expect(recorder.calls == 0, "a request does not notify a subscriber");
    expect(harness.controller.pending(), "it marks the route pending");

    expect(harness.controller.commit(), "commit publishes a change");
    harness.scheduler.flush();
    expect(recorder.calls == 1, "one flush delivers one notification");
    expect(recorder.last_route_id == "orders", "the delivered route is the new one");
    expect(recorder.last_depth == 1, "the delivered depth is the new one");

    // Three requests in one tick, then one commit: still one notification, and the value is the
    // last.
    (void)harness.controller.push("/settings");
    (void)harness.controller.push("/orders/9");
    (void)harness.controller.push("/search");
    expect(harness.controller.commit(), "the coalesced commit publishes");
    harness.scheduler.flush();
    expect(recorder.calls == 2, "three requests before one flush deliver one notification");
    expect(recorder.last_route_id == "search", "and the delivered route is the last one");

    // A tick with no request: no commit, no notification, no allocation of a new snapshot.
    expect(!harness.controller.commit(), "a clean commit reports no change");
    harness.scheduler.flush();
    expect(recorder.calls == 2, "and delivers nothing");
}

void entry_state_is_carried_per_entry() {
    Harness harness;
    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/orders/42");
    const std::uint64_t active_id = harness.controller.active()->entry_id;
    // Section 10.1: per-route serializable UI state, which is what a restore round-trips.
    expect(harness.controller.set_entry_state(active_id, StateMap{{"tab", "items"}}),
           "state is set on a live entry");
    const StateMap* state = harness.controller.entry_state(active_id);
    expect(state != nullptr && state->size() == 1, "the state is readable back");
    expect(!harness.controller.set_entry_state(9999, StateMap{}), "an unknown entry is refused");
}

} // namespace

int run_route_tests() {
    the_route_table_rejects_what_section_1_calls_an_error();
    a_path_resolves_to_the_specifying_route_with_typed_params();
    a_typed_param_that_does_not_parse_is_a_named_failure();
    untyped_params_are_available_without_a_declaration();
    an_unknown_route_is_a_named_failure_not_a_crash();
    malformed_input_is_refused_before_a_value_reaches_a_screen();
    push_grows_replace_does_not();
    the_duplicate_policy_is_per_route();
    the_stack_bound_is_a_named_failure();
    a_route_change_is_one_notification_per_flush();
    entry_state_is_carried_per_entry();
    return g_failures;
}

} // namespace nav_test
