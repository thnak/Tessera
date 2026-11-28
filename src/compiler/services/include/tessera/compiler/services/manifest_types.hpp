// SPDX-License-Identifier: MIT
//
// The data of a parsed `ui/services.json` (docs/specs/services-manifest-v1.md), with no parser and
// no dependency: the analysis layer (`tessera_react_rules`) takes these types without linking the
// JSON reader, so the Wasm designer build stays free of it.
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::services {

struct ManifestParam {
    std::string name;
    std::string type; // an IR type spelling, e.g. `string`, `optional<int>`, `Route`
};

struct ManifestMethod {
    std::string name;
    std::vector<ManifestParam> params;
    std::string returns; // an IR type spelling or `void`
};

// A service-owned `Observable<T>` TSX may read in render through `useObservable(svc.name)`
// (docs/specs/services-manifest-v1.md section 2). The C++ interface exposes it as
// `tessera::appfw::Observable<T>& name()` (subscribing registers an observer, so it is not const).
struct ManifestObservable {
    std::string name;
    std::string type; // an IR value type spelling (never `void`)
};

struct ManifestService {
    std::string id;   // reverse-DNS; the registry id is `service_id(id)`
    std::string name; // the TSX type name
    std::string cpp_type;
    std::string header;
    std::vector<ManifestMethod> methods;
    std::vector<ManifestObservable> observables;

    [[nodiscard]] const ManifestObservable*
    find_observable(std::string_view member) const noexcept {
        const auto found = std::ranges::find(observables, member, &ManifestObservable::name);
        return found == observables.end() ? nullptr : &*found;
    }
    [[nodiscard]] const ManifestMethod* find_method(std::string_view method) const noexcept {
        const auto found = std::ranges::find(methods, method, &ManifestMethod::name);
        return found == methods.end() ? nullptr : &*found;
    }
};

struct ServicesManifest {
    std::vector<ManifestService> services;

    [[nodiscard]] const ManifestService* find_name(std::string_view name) const noexcept {
        const auto found = std::ranges::find(services, name, &ManifestService::name);
        return found == services.end() ? nullptr : &*found;
    }
    [[nodiscard]] const ManifestService* find_id(std::string_view id) const noexcept {
        const auto found = std::ranges::find(services, id, &ManifestService::id);
        return found == services.end() ? nullptr : &*found;
    }
};

} // namespace tessera::compiler::services
