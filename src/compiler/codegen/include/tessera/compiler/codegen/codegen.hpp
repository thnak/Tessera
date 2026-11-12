// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

// First C++ codegen slice (docs/design/codegen.md): one `.hpp` and one `.cpp` per IR component.
// The generated translation unit is a pure function of the IR: same IR, same bytes.
namespace tessera::compiler::codegen {

struct GeneratedFile {
    // Deterministic: `<Component>.hpp` / `<Component>.cpp`, plus `<module.name>_stylesheets.hpp`
    // when the module has embedded stylesheet blobs.
    std::string path;
    std::string text;
};

// One compiled stylesheet of the module (issue #81, ADR-048): the serializer output of
// `tessera::ui::style::serialize_stylesheet`. Codegen never compiles CSS itself; the caller (the
// CLI) owns that, so the library keeps its IR-only dependency (AGENTS.md non-negotiable 4).
struct StylesheetEmbed {
    std::string path;                // must equal an ir::StylesheetRef::path
    std::vector<std::uint8_t> bytes; // serialize_stylesheet() output
};

// The project's theme asset as the CLI serialized it (issue #81, ADR-048;
// docs/specs/compiled-stylesheet-v1.md "Theme asset blob"). Codegen never parses the asset JSON:
// the caller owns `serialize_theme_asset()`, so the library keeps its IR-only dependency.
struct EmbeddedTheme {
    std::vector<std::uint8_t> bytes; // serialize_theme_asset() output
};

// One `TESSERA_CODEGEN_*` error. Lowering/codegen stops at the first unsupported construct; no
// partial file set is returned.
using CodegenErrors = std::vector<std::string>;
using CodegenResult = std::expected<std::vector<GeneratedFile>, CodegenErrors>;

// Generates the C++ files of every component of `module`, sorted by component name. The module name
// becomes the C++ namespace root of the generated structs.
//
// When at least one stylesheet of `module` has a blob in `embeds`, exactly one extra file
// `<module.name>_stylesheets.hpp` is appended (see docs/specs/compiled-stylesheet-v1.md section 9):
// the blobs as `inline constexpr` byte arrays plus the `kEmbeddedStylesheets` table, in
// `module.stylesheets` order. A sheet without a blob is skipped and keeps its warning. An embed
// whose `path` is not a stylesheet of the module is a `TESSERA_CODEGEN_STYLESHEET_UNKNOWN` error.
//
// When `theme` is set, exactly one more file `<module.name>_theme.hpp` is appended: the blob as an
// `inline constexpr` byte array and a `kEmbeddedTheme` record. No theme means no extra file and no
// new warning.
[[nodiscard]] CodegenResult generate(const ir::IrModule& module,
                                     std::span<const StylesheetEmbed> embeds = {},
                                     std::optional<EmbeddedTheme> theme = std::nullopt);

// Non-fatal findings for `module`, one `TESSERA_CG_*: ...` string each, in a deterministic order.
// Today: a stylesheet of the module that the caller supplied no blob for is
// `TESSERA_CG_STYLESHEET_NOT_EMBEDDED`, because generated code has no other way to deliver its CSS
// (issue #80/#81); an embedded sheet never warns.
[[nodiscard]] std::vector<std::string>
codegen_warnings(const ir::IrModule& module, std::span<const StylesheetEmbed> embeds = {});

} // namespace tessera::compiler::codegen
