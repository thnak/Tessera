// SPDX-License-Identifier: MIT

#pragma once

#include "project_identity.hpp"
#include "regenerate.hpp"
#include "regenerate_stylesheets.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The build-time asset embeds of the codegen CLI: the compiled stylesheet blobs and the theme asset
// (issue #81, ADR-048). Both are the same kind of concern — the CLI reads a file, parses it with
// the stylesheet engine, and hands codegen a serialized blob — and both are the only reason the CLI
// links `tessera_style`. They live in their own translation unit so the driver keeps to its job
// (parse, lower, validate, report, write) and `regenerate.cpp` stays within the file size rules.
namespace tessera::compiler::codegen::cli::internal {

// Compiled stylesheet blobs of one module, in the order the sheets were collected:
// `StylesheetEmbed::bytes` is exactly `serialize_stylesheet()` output, so the codegen library never
// sees a stylesheet engine header. The blob of a `*.module.css` compiles its scoped rewrite
// (issue #463), so its selectors are the ones the lowering puts on `className`. A stylesheet with a
// lexical error reports it and returns nullopt; a `::part(name)` selector naming a part no library
// component declares is a warning, not an error (stylesheet-v1 section 6.3).
[[nodiscard]] std::optional<std::vector<StylesheetEmbed>>
build_stylesheet_embeds(std::span<const StylesheetSource> sheets, Outcome& outcome);

// The embedded theme asset: a project without `assets.theme` has no asset and therefore no
// generated theme file. A configured asset is parsed, its `assets.themePins` checked and serialized
// once here, so the codegen library never sees the JSON (docs/specs/stylesheet-v1.md section 9.1).
// Returns false, having reported the diagnostic, when the asset cannot be read or parsed.
[[nodiscard]] bool build_theme_embed(const ProjectIdentity& identity, Outcome& outcome,
                                     std::optional<EmbeddedTheme>& theme);

} // namespace tessera::compiler::codegen::cli::internal
