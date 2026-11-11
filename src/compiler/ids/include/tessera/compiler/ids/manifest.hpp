// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <string>
#include <string_view>

namespace tessera::compiler::ids {

inline constexpr int kManifestVersion = 1;

// The automation manifest (`generated/automation.manifest.json`, docs/specs/automation-ids-v1.md
// section 2.1) of a module whose identity is assigned: the project salt, every component with its
// `componentId`, name and source file, and every node in document order with its `uiId`,
// `componentId`, kind, structural path and source position. Ids are decimal strings.
//
// It is a pure function of the module and the salt (keys in byte order, two-space indent, LF, one
// trailing newline), so the same source and config give a byte-identical file, and a diff of the
// manifest shows exactly which nodes were re-identified.
[[nodiscard]] std::string write_manifest(const ir::IrModule& module, std::string_view salt);

} // namespace tessera::compiler::ids
