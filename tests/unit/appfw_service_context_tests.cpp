// SPDX-License-Identifier: MIT
//
// AF1 (issue #173) part 1: `ServiceId` and `ServiceContext` under ADR-050 section 3.
// The state machine tests live in `appfw_service_runner_tests.cpp`.

#include "appfw_test_services.hpp"
#include "tessera/appfw/service_context.hpp"

namespace appfw_test {

using tessera::appfw::FrameScope;
using tessera::appfw::reset_service_lookup_audit;
using tessera::appfw::service_id;
using tessera::appfw::service_lookup_audit;
using tessera::appfw::ServiceContext;
using tessera::appfw::ServiceErrorCode;
using tessera::appfw::ServiceId;

int run_context_tests() {
    const int before = g_failures;

    // --- ServiceId is a compile-time value -----------------------------------------------------
    {
        constexpr ServiceId store = Store::kServiceId;
        constexpr ServiceId mail = Mail::kServiceId;
        expect(store.value != 0, "a service id is not zero");
        expect(store != mail, "distinct names hash to distinct ids");
        expect(store != service_id("tessera.test.storE"), "ids are case-sensitive");
    }

    // --- ServiceContext: provide, freeze, get, find ---------------------------------------------
    {
        ServiceContext scope;
        Store store;
        store.value = 7;
        Mail mail;

        expect(scope.provide<Store>(store).has_value(), "provide a first service");
        expect(scope.provide<Mail>(mail).has_value(), "provide a second service");
        expect(scope.size() == 2, "the registry size counts registrations");
        expect(!scope.frozen(), "a new registry is not frozen");
        expect(scope.find<Store>() == &store, "find resolves before freeze");

        expect(scope.freeze().has_value(), "freeze succeeds");
        expect(scope.frozen(), "freeze marks the registry frozen");
        expect(&scope.get<Store>() == &store, "get returns the registered instance");
        expect(scope.get<Store>().value == 7, "get sees the instance's state");
        expect(scope.find<Mail>() == &mail, "find returns the registered instance");
        expect(scope.find<UnknownId>() == nullptr, "find returns null for an unregistered id");
        expect(scope.contains(Store::kServiceId), "contains reports a registered id");
        expect(!scope.contains(UnknownId::kServiceId), "contains reports an absent id");
        expect(scope.freeze().has_value(), "freeze is idempotent");

        Mail other;
        const auto late = scope.provide<Mail>(other);
        expect(!late.has_value(), "provide after freeze is rejected");
        expect(late.error().code == ServiceErrorCode::Frozen, "late provide reports Frozen");
    }

    // --- ServiceContext: duplicate and capacity -------------------------------------------------
    {
        ServiceContext scope(2);
        Store store;
        expect(scope.provide<Store>(store).has_value(), "provide into a sized registry");
        const auto again = scope.provide<Store>(store);
        expect(!again.has_value(), "a duplicate id is rejected");
        expect(again.error().code == ServiceErrorCode::DuplicateId,
               "duplicate reports DuplicateId");

        Mail mail;
        expect(scope.provide<Mail>(mail).has_value(), "fill the registry to capacity");
        expect(scope.size() == scope.capacity(), "the registry is full");
        UnknownId other;
        const auto overflow = scope.provide<UnknownId>(other);
        expect(!overflow.has_value(), "a full registry rejects a new id");
        expect(overflow.error().code == ServiceErrorCode::CapacityExceeded,
               "overflow reports CapacityExceeded");
    }

    // --- ServiceContext: the parent chain (app scope + window scope) -----------------------------
    {
        ServiceContext app_scope;
        Store store;
        expect(app_scope.provide<Store>(store).has_value(), "provide into the app scope");
        expect(app_scope.freeze().has_value(), "freeze the app scope");

        ServiceContext window_scope(ServiceContext::kDefaultCapacity, &app_scope);
        Mail mail;
        expect(window_scope.provide<Mail>(mail).has_value(), "provide into the window scope");
        expect(window_scope.freeze().has_value(), "freeze the window scope");

        expect(window_scope.find<Store>() == &store, "a child scope sees the parent's service");
        expect(window_scope.find<Mail>() == &mail, "a child scope sees its own service");
        expect(app_scope.find<Mail>() == nullptr, "a parent does not see the child's service");
        expect(window_scope.parent() == &app_scope, "the parent chain is recorded");
        expect(app_scope.parent() == nullptr, "a root scope has no parent");
    }

    // --- the in-frame lookup counter -------------------------------------------------------------
    {
        reset_service_lookup_audit();
        ServiceContext scope;
        Store store;
        expect(scope.provide<Store>(store).has_value(), "provide for the audit test");
        expect(scope.freeze().has_value(), "freeze for the audit test");

        expect(scope.get<Store>().value == store.value, "a legal lookup resolves");
        expect(scope.find<Store>() == &store, "a legal find resolves");
        expect(service_lookup_audit().in_frame == 0, "legal lookups are not counted as in-frame");

        {
            FrameScope frame;
            expect(FrameScope::active(), "a frame scope is active while it lives");
            (void)scope.find<Store>();
            (void)scope.find<Store>();
            expect(service_lookup_audit().in_frame == 2, "in-frame lookups are counted");
        }
        expect(!FrameScope::active(), "the frame scope is inactive after it ends");

        // Nesting is counted, so an inner stage mark cannot cancel an outer frame mark.
        {
            FrameScope outer;
            {
                FrameScope inner;
                expect(FrameScope::active(), "nesting keeps the frame active");
            }
            expect(FrameScope::active(), "the outer frame mark survives the inner one");
            (void)scope.find<Store>();
            expect(service_lookup_audit().in_frame == 3, "the outer frame still counts lookups");
        }
        expect(service_lookup_audit().in_frame == 3, "leaving a nested frame does not recount");

        const auto missing_before = service_lookup_audit().missing;
        (void)scope.find<UnknownId>();
        expect(service_lookup_audit().missing == missing_before + 1, "a miss is counted");

        reset_service_lookup_audit();
        expect(service_lookup_audit().in_frame == 0, "the in-frame counter can be reset");
        expect(service_lookup_audit().missing == 0, "the miss counter can be reset");
    }

    return g_failures - before;
}

} // namespace appfw_test
