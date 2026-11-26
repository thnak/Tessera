// SPDX-License-Identifier: MIT
//
// The `ui/services.json` reader (docs/specs/services-manifest-v1.md; ADR-050 section 3.1 item 1;
// issue #177). It validates the file against the spec's schema and produces plain data; it does not
// read the C++ header, resolve record names against a module, or touch the IR. The caller that has
// a module checks that every record type named here is declared (`TESSERA_SVC_MANIFEST_TYPE`).
#pragma once

#include "tessera/compiler/services/manifest_types.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::compiler::services {

inline constexpr int kManifestVersion = 1;

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

// Reads `<root>/ui/services.json` (spec section 1). A project with no such file has no services, so
// an absent file is a value (`nullopt`), not an error; a file that exists and does not read or
// parse is the error. The code of an unreadable file is `TESSERA_SVC_MANIFEST_SYNTAX`.
[[nodiscard]] std::expected<std::optional<ServicesManifest>, ManifestError>
load_services_manifest(const std::filesystem::path& root);

// Whether `type` is a spelling the spec allows for a parameter (`allow_void` false) or a return
// (`allow_void` true). Record names are accepted by shape; the module check resolves them.
[[nodiscard]] bool is_manifest_type(std::string_view type, bool allow_void);

} // namespace tessera::compiler::services
