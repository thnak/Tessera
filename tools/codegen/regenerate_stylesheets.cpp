// SPDX-License-Identifier: MIT

// Stylesheet collection of the codegen CLI (issues #80, #463): which CSS files a run embeds, and
// which text each embedded blob compiles.

#include "regenerate_stylesheets.hpp"
#include "tessera/compiler/css_module/module_scan.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace fs = std::filesystem;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;

// The design's gate is the file's own suffix, not the form of the import
// (docs/design/css-isolation.md): a `*.module.css` is scoped at compile time whoever imports it.
// Keying on the path also makes `blob_text` a function of (path, bytes) alone, so a file imported
// both for effect and for its class map yields the same single entry in either order.
constexpr std::string_view kModuleSuffix = ".module.css";

// The file an importer names, with the driver's rule for a command-line input: relative to
// `base_dir` when the caller gave one (the MCP server), else as the run spells the path.
fs::path importer_on_disk(const Options& options, std::string_view importer) {
    return options.base_dir.empty() ? fs::path(importer)
                                    : fs::path(options.base_dir) / fs::path(importer);
}

bool already_collected(const std::vector<StylesheetSource>& sheets, std::string_view path) {
    return std::any_of(sheets.begin(), sheets.end(),
                       [&](const StylesheetSource& sheet) { return sheet.path == path; });
}

} // namespace

bool collect_stylesheets(std::span<const std::unique_ptr<detail::SourceFile>> sources,
                         const Options& options, const ProjectIdentity& identity,
                         bool project_paths, std::vector<StylesheetSource>& sheets,
                         Outcome& outcome) {
    for (const std::unique_ptr<detail::SourceFile>& file : sources) {
        const detail::SourceFile& source = *file;
        if (!source.parses()) {
            continue; // the driver reports a parse failure before it asks for stylesheets
        }
        for (const jsx::ModuleItem& item : source.parsed.root->items) {
            // A CSS module is collected whether it is imported for its class map or only for
            // effect (issue #463): either way the page's styles come from this file.
            if (item.kind != jsx::ItemKind::Import ||
                (!item.side_effect && item.css_module_binding.empty())) {
                continue;
            }
            const std::size_t slash = source.path.rfind('/');
            std::string path = lowering::join_project_path(
                slash == std::string::npos ? std::string_view()
                                           : std::string_view(source.path).substr(0, slash),
                item.source);
            if (already_collected(sheets, path)) {
                continue;
            }
            const fs::path on_disk =
                project_paths
                    ? identity.root / fs::path(path)
                    : (importer_on_disk(options, source.path).parent_path() / fs::path(item.source))
                          .lexically_normal();
            std::optional<std::string> text = read_file(on_disk);
            if (!text) {
                // A missing file stays the lowering's error at the import's span
                // (TESSERA_IR_STYLESHEET_MISSING); an existing file the CLI cannot read is a build
                // failure, because nothing else would report it.
                std::error_code exists_error;
                if (fs::exists(on_disk, exists_error)) {
                    report(outcome, true,
                           path + ":1:1: TESSERA_CSS_READ: cannot read the stylesheet `" + path +
                               "`");
                    outcome.exit_code = 1;
                    return false;
                }
                continue;
            }
            std::string blob_text = *text;
            if (path.ends_with(kModuleSuffix)) {
                // The blob must define the scoped names the lowering puts on `className`; the
                // stylesheet text the lowering hashes stays the file's own bytes (issue #463).
                const auto scan = css_module::scan_module_css(path, *text);
                if (!scan) {
                    report(outcome, true,
                           path + ":1:1: " + scan.error().code + ": " + scan.error().message);
                    outcome.exit_code = 1;
                    return false;
                }
                blob_text = scan->rewritten;
            }
            sheets.push_back(
                StylesheetSource{std::move(path), std::move(*text), std::move(blob_text)});
        }
    }
    return true;
}

} // namespace tessera::compiler::codegen::cli::internal
