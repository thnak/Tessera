// SPDX-License-Identifier: MIT
//
// The service interfaces the shim packages lower to (ADR-058 class B, docs/specs/router-shim-v1.md
// section 4). They are always visible to `useService<T>()`, so a project needs no
// `ui/services.json` entry for them; a manifest entry of the same name wins, which is how a project
// substitutes its own interface. Header-only like `manifest_types.hpp`, so the analysis layer links
// nothing new.
#pragma once

#include "tessera/compiler/services/manifest_types.hpp"

namespace tessera::compiler::services {

// `tessera::appfw::RouterService` (include/tessera/appfw/router_service.hpp), the `navigate` of
// the `react-router-dom` shim.
[[nodiscard]] inline const ManifestService& builtin_router_service() {
    static const ManifestService service = [] {
        ManifestService router;
        router.id = "tessera.router";
        router.name = "RouterService";
        router.cpp_type = "tessera::appfw::RouterService";
        router.header = "tessera/appfw/router_service.hpp";
        router.methods.push_back({"push", {{"path", "string"}}, "void"});
        router.methods.push_back({"replace", {{"path", "string"}}, "void"});
        router.methods.push_back({"back", {}, "void"});
        return router;
    }();
    return service;
}

// The built-in service named `name`, or null.
[[nodiscard]] inline const ManifestService* find_builtin_service(std::string_view name) noexcept {
    const ManifestService& router = builtin_router_service();
    return name == router.name ? &router : nullptr;
}

} // namespace tessera::compiler::services
