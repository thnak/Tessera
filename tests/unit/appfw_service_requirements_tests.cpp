// SPDX-License-Identifier: MIT
//
// ADR-050 section 3.1 item 6: `Application::require_services` makes `launch()` refuse to start
// while a required service has no provider, and start once it has one. An application that never
// called it is unchecked. Deterministic: a ManualClock, no wall clock, no threads.

#include "tessera/appfw/application.hpp"
#include "tessera/appfw/service_requirements.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/runtime/clock.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace {

using tessera::appfw::Application;
using tessera::appfw::RequiredService;
using tessera::appfw::ServiceContext;
using tessera::appfw::ServiceId;
using tessera::appfw::ServiceRunner;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_service_requirements: " << message << "\n";
        ++failures;
    }
}

struct Provided {
    static constexpr ServiceId kServiceId = tessera::appfw::service_id("tessera.test.Provided");
};

struct Absent {
    static constexpr ServiceId kServiceId = tessera::appfw::service_id("tessera.test.Absent");
};

} // namespace

int main() {
    const std::array<RequiredService, 2> required{
        RequiredService{Provided::kServiceId, "Provided", "Inbox", "ui/Inbox.tsx:3:9"},
        RequiredService{Absent::kServiceId, "Absent", "Inbox", "ui/Inbox.tsx:4:9"}};

    tessera::runtime::ManualClock clock;
    tessera::runtime::CompletionQueue queue{16};
    ServiceRunner runner{clock, queue, nullptr};
    expect(runner.prepare().has_value(), "the runner prepares");

    ServiceContext context;
    Provided provided;
    expect(context.provide<Provided>(provided).has_value(), "the service is provided");
    expect(context.freeze().has_value(), "the scope freezes");

    // The count is exact, and the buffer holds the first missing entry only.
    std::array<RequiredService, 1> missing;
    expect(tessera::appfw::find_missing_services(context, required, missing) == 1,
           "exactly one requirement is missing");
    expect(missing[0].type == "Absent" && missing[0].source == "ui/Inbox.tsx:4:9",
           "the missing entry names the interface and the TSX location");

    {
        Application checked{runner};
        checked.require_services(context, required);
        expect(!checked.launch(), "launch is refused while a required service is missing");
        expect(!checked.launched(), "a refused launch leaves the app unlaunched");
    }
    {
        Application checked{runner};
        checked.require_services(context, std::span<const RequiredService>(required).first(1));
        expect(checked.launch(), "launch proceeds once every requirement has a provider");
    }
    {
        Application unchecked{runner};
        expect(unchecked.launch(), "an app that never called require_services is unchecked");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "appfw service requirements tests passed\n";
    return 0;
}
