// SPDX-License-Identifier: MIT
//
// WM_GETOBJECT host wiring: the root-provider handshake, the not-ready fallback, and the shutdown
// order. A fake apartment and a fake platform seam stand in for COM/UIAutomationCore, so the test
// is deterministic and needs no desktop session or UIA client.

#include "a11y_uia_host_fixture.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

namespace {

using namespace a11y_uia_host_test;
using tessera::ui::a11y::kUiaRootObjectId;
using tessera::ui::a11y::UiaError;
using tessera::ui::a11y::UiaHost;
using tessera::ui::a11y::UiaHostConfig;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y uia host: " << message << "\n";
    }
    return condition;
}

bool expect_eq(std::int64_t actual, std::int64_t expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y uia host: " << message << " (actual=" << actual << ", expected=" << expected
              << ")\n";
    return false;
}

struct Harness {
    Log log;
    LoggingApartment* apartment = nullptr;
    FakePlatform* platform = nullptr;
    std::unique_ptr<UiaHost> host;
};

std::unique_ptr<Harness> make_harness(void* root = reinterpret_cast<void*>(0x1234)) {
    auto harness = std::make_unique<Harness>();
    auto apartment = std::make_unique<LoggingApartment>(harness->log);
    harness->apartment = apartment.get();
    auto platform = std::make_unique<FakePlatform>(harness->log, root);
    harness->platform = platform.get();
    UiaHostConfig config;
    // A long idle wait keeps the worker asleep unless a `step` barrier wakes it, so the assertions
    // never depend on thread scheduling.
    config.bridge.idle_wait_ms = 1000;
    config.bridge.shutdown_wait_ms = 5000;
    harness->host = std::make_unique<UiaHost>(config, std::move(apartment), std::move(platform));
    return harness;
}

bool not_ready_before_start() {
    auto harness = make_harness();
    void* raw = nullptr;
    return expect(!harness->host->ready(), "not ready before start") &&
           expect(!harness->host->answer_get_object(kUiaRootObjectId, raw),
                  "no answer before start") &&
           expect(raw == nullptr, "the answer leaves the provider null");
}

bool handshake_publishes_the_bridge_root() {
    auto harness = make_harness();
    UiaHost& host = *harness->host;
    if (!expect(host.start().has_value(), "start succeeds")) {
        return false;
    }
    if (!expect(host.bridge().step().has_value(), "step is a barrier behind the start hook")) {
        return false;
    }
    void* raw = nullptr;
    const auto log = harness->log.snapshot();
    const std::int64_t init = index_of(log, "apartment.init");
    const std::int64_t create = index_of(log, "platform.create");
    return expect(host.ready(), "the root is published") &&
           expect(host.answer_get_object(kUiaRootObjectId, raw), "the root answers WM_GETOBJECT") &&
           expect(raw == reinterpret_cast<void*>(0x1234), "the published pointer is the root") &&
           expect(harness->platform->create_thread == host.bridge().bridge_thread(),
                  "the root is created on the bridge thread") &&
           expect(!harness->platform->running_at_create,
                  "the root is published before the bridge reports Running") &&
           expect(init >= 0 && create > init, "the root is created after the apartment exists") &&
           expect_eq(static_cast<std::int64_t>(harness->log.count("platform.create")), 1,
                     "one root per start");
}

bool only_the_uia_root_object_id_is_answered() {
    auto harness = make_harness();
    UiaHost& host = *harness->host;
    if (!expect(host.start().has_value(), "start succeeds") ||
        !expect(host.bridge().step().has_value(), "step barrier")) {
        return false;
    }
    void* raw = nullptr;
    constexpr std::intptr_t kMsaaClientObjectId = -4; // OBJID_CLIENT
    return expect(!host.answer_get_object(kMsaaClientObjectId, raw),
                  "OBJID_CLIENT keeps the MSAA default bridge") &&
           expect(!host.answer_get_object(0, raw), "a zero object id is not the UIA root") &&
           expect(raw == nullptr, "a rejected object id writes nothing");
}

bool answer_never_waits_for_the_bridge() {
    auto harness = make_harness();
    UiaHost& host = *harness->host;
    // Park the worker inside its message pump: the start hook has already run by then, so the
    // answer must still be served from the published pointer alone.
    harness->apartment->block_pump();
    if (!expect(host.start().has_value(), "start succeeds") ||
        !expect(host.bridge().step().has_value(), "step completes while the pump is parked")) {
        harness->apartment->open_pump();
        return false;
    }
    void* raw = nullptr;
    const bool answered = host.answer_get_object(kUiaRootObjectId, raw);
    const bool published = raw == reinterpret_cast<void*>(0x1234);
    harness->apartment->open_pump();
    host.stop();
    return expect(answered, "the root answers while the worker is parked") &&
           expect(published, "the parked worker does not change the answer") &&
           expect(!host.ready(), "stop clears the root after the gate opens");
}

bool stop_clears_then_releases_before_apartment_shutdown() {
    auto harness = make_harness();
    UiaHost& host = *harness->host;
    if (!expect(host.start().has_value(), "start succeeds") ||
        !expect(host.bridge().step().has_value(), "step barrier")) {
        return false;
    }
    host.stop();
    void* raw = nullptr;
    const auto log = harness->log.snapshot();
    const std::int64_t destroy = index_of(log, "platform.destroy");
    const std::int64_t shutdown = index_of(log, "apartment.shutdown");
    return expect(!host.ready(), "stop clears the root") &&
           expect(!host.answer_get_object(kUiaRootObjectId, raw), "a stopped host falls back") &&
           expect_eq(static_cast<std::int64_t>(harness->platform->destroys_count()), 1,
                     "the root is released once") &&
           expect(harness->platform->destroy_thread == harness->platform->create_thread,
                  "the root is released on the thread that created it") &&
           expect(destroy >= 0 && shutdown > destroy,
                  "the root is released before the apartment shuts down") &&
           expect(host.bridge().phase() == tessera::ui::a11y::UiaBridgePhase::Stopped,
                  "the bridge reports Stopped");
}

bool failed_apartment_never_creates_a_root() {
    auto harness = make_harness();
    UiaHost& host = *harness->host;
    harness->apartment->fail_init_with(UiaError::ApartmentUnavailable);
    if (!expect(host.start().has_value(), "start is non-blocking and succeeds")) {
        return false;
    }
    const auto stepped = host.bridge().step();
    void* raw = nullptr;
    const auto log = harness->log.snapshot();
    return expect(!stepped.has_value(), "step reports the apartment failure") &&
           expect(stepped.error() == UiaError::ApartmentUnavailable, "the error is the value") &&
           expect(!host.ready(), "a failed bridge publishes no root") &&
           expect(!host.answer_get_object(kUiaRootObjectId, raw), "the window falls back") &&
           expect(index_of(log, "apartment.init") >= 0, "the apartment was attempted") &&
           expect(index_of(log, "platform.create") < 0, "no root without an apartment") &&
           expect(index_of(log, "apartment.shutdown") < 0,
                  "a failed init is not balanced with a shutdown");
}

bool restart_publishes_and_releases_again() {
    auto harness = make_harness();
    UiaHost& host = *harness->host;
    if (!expect(host.start().has_value(), "first start") ||
        !expect(host.bridge().step().has_value(), "first barrier") ||
        !expect(host.ready(), "first root")) {
        return false;
    }
    host.stop();
    if (!expect(host.start().has_value(), "restart succeeds") ||
        !expect(host.bridge().step().has_value(), "restart barrier") ||
        !expect(host.ready(), "the restarted bridge publishes a root")) {
        return false;
    }
    host.stop();
    return expect_eq(static_cast<std::int64_t>(harness->log.count("platform.create")), 2,
                     "one root per start") &&
           expect_eq(static_cast<std::int64_t>(harness->platform->destroys_count()), 2,
                     "one release per stop");
}

#if !defined(_WIN32)
bool unsupported_platform_is_an_expected_value() {
    // Every non-Windows build has no UIA platform seam: accessibility reports disabled instead of
    // failing the app.
    auto harness = make_harness();
    UiaHostConfig config;
    auto apartment = std::make_unique<LoggingApartment>(harness->log);
    UiaHost host(config, std::move(apartment), nullptr);
    const auto started = host.start();
    return expect(!started.has_value(), "start reports a value") &&
           expect(started.error() == UiaError::UnsupportedPlatform,
                  "UnsupportedPlatform on a platform without UIA");
}
#endif

} // namespace

int main() {
    if (!not_ready_before_start()) {
        return 1;
    }
    if (!handshake_publishes_the_bridge_root()) {
        return 2;
    }
    if (!only_the_uia_root_object_id_is_answered()) {
        return 3;
    }
    if (!answer_never_waits_for_the_bridge()) {
        return 4;
    }
    if (!stop_clears_then_releases_before_apartment_shutdown()) {
        return 5;
    }
    if (!failed_apartment_never_creates_a_root()) {
        return 6;
    }
    if (!restart_publishes_and_releases_again()) {
        return 7;
    }
#if !defined(_WIN32)
    if (!unsupported_platform_is_an_expected_value()) {
        return 8;
    }
#endif
    std::cout << "a11y_uia_host_getobject: ok\n";
    return 0;
}
