// SPDX-License-Identifier: MIT

// Project-level stylesheet resolution (issue #80) and CSS module scanning (issue #463). This lives
// outside module_lowering.cpp so that file stays inside the source-size gate; it owns the same
// policy: the caller reads the files, lowering resolves, deduplicates and sorts.

#pragma once

#include "tessera/compiler/lowering/node_lowering.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::lowering::internal {

// "ui/App.tsx" -> "ui"; empty for a file at the project root.
[[nodiscard]] std::string directory_of(std::string_view file);

// "ui/Card.module.css" -> "ui/Card.module"; a path with no extension is unchanged.
[[nodiscard]] std::string without_extension(std::string_view path);

// One diagnostic span resolved through `source`'s map (empty when the source has none).
[[nodiscard]] jsx::Diagnostic make_diagnostic(const ModuleSource& source, jsx::Severity severity,
                                              std::string_view code, std::string message,
                                              jsx::Span span, std::string_view suggestion = {});

// One `*.module.css` sheet the project imported as a CSS module: the class map its scan produced
// and the import that bound it, which is where an unused-class warning points.
struct ScannedModule {
    std::string path;
    std::string hash;                         // 6 hex digits of the path FNV-1a
    std::vector<ir::StylesheetClass> classes; // sorted by local, unique
    const ModuleSource* source = nullptr;
    jsx::Span span{};
};

// The stylesheets of one project. `stylesheets` is deduplicated by path and unsorted; the caller
// sorts it for the IR.
class StylesheetSet {
public:
    // Resolves one CSS import of `source`: a side-effect import records the sheet, and a module
    // import (non-empty `item.css_module_binding`) also scans it and appends the class map to
    // `bindings`. Nullopt on success, else the diagnostic to report.
    [[nodiscard]] std::optional<jsx::Diagnostic> resolve(const ModuleSource& source,
                                                         const jsx::ModuleItem& item,
                                                         std::span<const StylesheetFile> files,
                                                         std::vector<CssModuleBinding>& bindings);

    // The first two module sheets whose 24-bit path hashes collide, ordered by path. A collision is
    // a compile error because a silent merge would restyle one component with the other's rules.
    [[nodiscard]] std::optional<jsx::Diagnostic> check_collisions() const;

    // Every class of a module sheet that `uses` never names, ordered by path then class.
    [[nodiscard]] std::vector<LoweringWarning>
    unused_classes(std::span<const CssModuleUse> uses) const;

    std::vector<ir::StylesheetRef> stylesheets;

private:
    [[nodiscard]] const ScannedModule* find_module(std::string_view path) const;

    // One entry per path, kept sorted by path so every pass over it is deterministic.
    std::vector<ScannedModule> modules_;
};

} // namespace tessera::compiler::lowering::internal
