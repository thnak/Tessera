// SPDX-License-Identifier: MIT
//
// Startup validation of the services a compiled component requires (ADR-050 section 3.1 item 6,
// docs/specs/services-manifest-v1.md section 3 item 6). A generated component exposes its
// `useService<T>()` requirements as a static list; the composition root concatenates the lists of
// the components it mounts and checks them against the frozen scopes once, before any mount, so a
// missing provider is reported with the component and the TSX location instead of aborting inside
// the first `ServiceContext::get<T>()`. Header-only, no allocation, no RTTI.
#pragma once

#include "tessera/appfw/service_context.hpp"
#include "tessera/appfw/service_id.hpp"

#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

namespace tessera::appfw {

// One `useService<T>()` of one component. The strings are literals owned by the generated code.
struct RequiredService {
    ServiceId id{};
    std::string_view type;      // the C++ interface, e.g. `demo::Navigator`
    std::string_view component; // the declaring component
    std::string_view source;    // the TSX `file:line:column` of the `useService`
};

// Copies every requirement `context` (or a parent scope) cannot resolve into `missing`, in input
// order, and returns how many there are. The count may exceed `missing.size()`; only the first
// `missing.size()` are stored, so a caller sizes the buffer to the requirement list for a full
// report. Zero means the app may mount.
[[nodiscard]] inline std::size_t
find_missing_services(const ServiceContext& context, std::span<const RequiredService> required,
                      std::span<RequiredService> missing) noexcept {
    std::size_t count = 0;
    for (const RequiredService& requirement : required) {
        if (context.contains(requirement.id)) {
            continue;
        }
        if (count < missing.size()) {
            missing[count] = requirement;
        }
        ++count;
    }
    return count;
}

// One `TESSERA_APPFW_SERVICE_MISSING` line per entry, naming the interface, the component and the
// TSX location. The caller decides whether to terminate.
inline void report_missing_services(std::span<const RequiredService> missing) noexcept {
    for (const RequiredService& requirement : missing) {
        std::fprintf(stderr,
                     "TESSERA_APPFW_SERVICE_MISSING: component `%.*s` requires `%.*s` (%.*s) but "
                     "no provider is registered\n",
                     static_cast<int>(requirement.component.size()), requirement.component.data(),
                     static_cast<int>(requirement.type.size()), requirement.type.data(),
                     static_cast<int>(requirement.source.size()), requirement.source.data());
    }
}

} // namespace tessera::appfw
