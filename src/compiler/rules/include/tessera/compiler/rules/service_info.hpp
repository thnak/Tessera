// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

#include <cstddef>
#include <string>

namespace tessera::compiler::rules {

// One `const nav = useService<Navigator>();` statement in its accepted form (ADR-050 section 3.1,
// issue #177). `decl` points into the manifest the analysis was given, which must outlive the
// analysis; it is never null for a recorded use, because an interface the manifest does not list is
// a diagnostic and contributes no entry.
struct ServiceUseInfo {
    std::size_t statement = 0;
    std::string name; // the binding (`nav`): what the accessor and the `svc::` id are named after
    std::string service; // the TSX type argument (`Navigator`)
    const services::ManifestService* decl = nullptr;
    jsx::Span span;
};

} // namespace tessera::compiler::rules
