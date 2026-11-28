// SPDX-License-Identifier: MIT

#include "tessera/compiler/analysis/project_index.hpp"

#include <algorithm>
#include <map>

namespace tessera::compiler::analysis {

namespace {

bool less(const Diagnostic& a, const Diagnostic& b) {
    if (a.span.file != b.span.file) {
        return a.span.file < b.span.file;
    }
    if (a.span.from != b.span.from) {
        return a.span.from < b.span.from;
    }
    if (a.span.to != b.span.to) {
        return a.span.to < b.span.to;
    }
    return a.code != b.code ? a.code < b.code : a.message < b.message;
}

bool has_errors(const FileIndex& index) {
    return std::ranges::any_of(index.diagnostics, [](const Diagnostic& d) {
        return d.severity == DiagnosticSeverity::Error;
    });
}

} // namespace

std::vector<Diagnostic> ProjectIndex::diagnostics(std::optional<FileId> only) const {
    std::vector<Diagnostic> out;
    // Top-level component declarations by name, in file id then source order.
    std::map<std::string, std::vector<const Symbol*>> components;
    for (const auto& [id, entry] : entries_) {
        out.insert(out.end(), entry.index.diagnostics.begin(), entry.index.diagnostics.end());
        for (const Symbol& symbol : entry.index.symbols) {
            if (symbol.kind == SymbolKind::Component && symbol.parent.empty()) {
                components[symbol.name].push_back(&symbol);
            }
            if (!symbol.import_source.starts_with(".") || !entry.index.indexed) {
                continue;
            }
            if (symbol.kind == SymbolKind::CssModule) {
                // A warning: a snapshot may legitimately carry only the source files.
                if (!resolve_path(id, symbol.import_source)) {
                    out.push_back(
                        Diagnostic{symbol.span,
                                   DiagnosticSeverity::Warning,
                                   "TESSERA_ANALYSIS_IMPORT_FILE",
                                   "no file in the project matches '" + symbol.import_source + "'",
                                   std::nullopt,
                                   {}});
                }
            } else if (symbol.kind == SymbolKind::Import && !symbol.target.resolved() &&
                       symbol.target.reason == Unresolved::ImportUnresolved) {
                const std::optional<FileId> target = resolve_path(id, symbol.import_source);
                if (!target) {
                    out.push_back(
                        Diagnostic{symbol.span,
                                   DiagnosticSeverity::Error,
                                   "TESSERA_ANALYSIS_IMPORT_FILE",
                                   "no file in the project matches '" + symbol.import_source + "'",
                                   std::nullopt,
                                   {}});
                } else if (!has_errors(entries_.at(*target).index)) {
                    // A file that did not parse cleanly may have lost the export; its own
                    // diagnostics are the cause, so the import is not reported on top of them.
                    out.push_back(Diagnostic{symbol.span,
                                             DiagnosticSeverity::Error,
                                             "TESSERA_ANALYSIS_IMPORT_NAME",
                                             "'" + symbol.import_source + "' does not export '" +
                                                 symbol.import_name + "'",
                                             std::nullopt,
                                             {}});
                }
            }
        }
    }
    for (const auto& [name, declared] : components) {
        for (std::size_t i = 0; declared.size() > 1 && i < declared.size(); ++i) {
            const Symbol& symbol = *declared[i];
            const Symbol& other = *declared[i == 0 ? 1 : 0];
            const std::string path = entries_.at(other.span.file).source.path;
            out.push_back(Diagnostic{symbol.span, DiagnosticSeverity::Error,
                                     "TESSERA_ANALYSIS_DUPLICATE_COMPONENT",
                                     "component '" + name + "' is also declared in " +
                                         (path.empty() ? "another file" : "'" + path + "'"),
                                     other.span, "the other declaration"});
        }
    }
    if (only) {
        std::erase_if(out, [&](const Diagnostic& d) { return d.span.file != *only; });
    }
    std::ranges::stable_sort(out, less);
    return out;
}

} // namespace tessera::compiler::analysis
