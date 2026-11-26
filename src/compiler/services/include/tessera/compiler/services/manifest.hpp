// SPDX-License-Identifier: MIT
//
// The `ui/services.json` reader (docs/specs/services-manifest-v1.md; ADR-050 section 3.1 item 1;
// issue #177). It validates the file against the spec's schema and produces plain data; it does not
// read the C++ header, resolve record names against a module, or touch the IR. The caller that has
// a module checks that every record type named here is declared (`TESSERA_SVC_MANIFEST_TYPE`).
#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::services {

inline constexpr int kManifestVersion = 1;

struct ManifestParam {
    std::string name;
    std::string type; // an IR type spelling, e.g. `string`, `optional<int>`, `Route`
};

struct ManifestMethod {
    std::string name;
    std::vector<ManifestParam> params;
    std::string returns; // an IR type spelling or `void`
};

struct ManifestService {
    std::string id;   // reverse-DNS; the registry id is `service_id(id)`
    std::string name; // the TSX type name
    std::string cpp_type;
    std::string header;
    std::vector<ManifestMethod> methods;

    [[nodiscard]] const ManifestMethod* find_method(std::string_view method) const noexcept;
};

struct ServicesManifest {
    std::vector<ManifestService> services;

    [[nodiscard]] const ManifestService* find_name(std::string_view name) const noexcept;
    [[nodiscard]] const ManifestService* find_id(std::string_view id) const noexcept;
};

// One failure: a stable code from the spec's section 4, the JSON pointer of the offending value
// (`/services/1/methods/0/returns`) and the byte offset in the text (0 when the text itself did not
// parse at a known place).
struct ManifestError {
    std::string code;
    std::string pointer;
    std::string message;
    std::size_t offset = 0;
};

// The first error wins: a manifest is small and a second diagnostic is usually the first one again.
[[nodiscard]] std::expected<ServicesManifest, ManifestError>
parse_services_manifest(std::string_view text);

// Whether `type` is a spelling the spec allows for a parameter (`allow_void` false) or a return
// (`allow_void` true). Record names are accepted by shape; the module check resolves them.
[[nodiscard]] bool is_manifest_type(std::string_view type, bool allow_void);

} // namespace tessera::compiler::services
