// SPDX-License-Identifier: MIT

// Project-level stylesheet resolution (issue #80) and CSS module scanning (issue #463): the caller
// reads every file, this pass resolves the imports, deduplicates by path and builds the class maps
// the node lowering reads.

#include "module_css.hpp"
#include "tessera/compiler/css_module/module_scan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

// The scan reports its code as a std::string; a diagnostic keeps a string_view, so the code must be
// one of the stable literals from compiler-diagnostics-v1.md section 3.1.
std::string_view stable_scan_code(std::string_view code) {
    if (code == "TESSERA_CSS_MODULE_GLOBAL_BLOCK") {
        return "TESSERA_CSS_MODULE_GLOBAL_BLOCK";
    }
    if (code == "TESSERA_CSS_MODULE_MALFORMED_GLOBAL") {
        return "TESSERA_CSS_MODULE_MALFORMED_GLOBAL";
    }
    return "TESSERA_CSS_MODULE_MALFORMED";
}

std::vector<ir::StylesheetClass> to_classes(const std::vector<css_module::ModuleClass>& classes) {
    std::vector<ir::StylesheetClass> out;
    out.reserve(classes.size());
    for (const css_module::ModuleClass& item : classes) {
        out.push_back(ir::StylesheetClass{item.local, item.scoped});
    }
    return out;
}

// FNV-1a 64 over the file bytes: deterministic, dependency-free, and enough to detect a changed
// stylesheet (it is a change marker, not a security digest).
std::string stylesheet_hash(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out = "fnv1a64:";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kHex[(hash >> shift) & 0xF]);
    }
    return out;
}

} // namespace

std::string directory_of(std::string_view file) {
    const std::size_t slash = file.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(file.substr(0, slash));
}

std::string without_extension(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return std::string(path);
    }
    return std::string(path.substr(0, dot));
}

jsx::Diagnostic make_diagnostic(const ModuleSource& source, jsx::Severity severity,
                                std::string_view code, std::string message, jsx::Span span,
                                std::string_view suggestion) {
    return jsx::Diagnostic{code,
                           severity,
                           std::move(message),
                           source.source_map != nullptr ? source.source_map->resolve(span)
                                                        : jsx::ResolvedSpan{},
                           std::string(suggestion),
                           std::nullopt};
}

const ScannedModule* StylesheetSet::find_module(std::string_view path) const {
    const auto found = std::lower_bound(
        modules_.begin(), modules_.end(), path,
        [](const ScannedModule& module, std::string_view value) { return module.path < value; });
    return found != modules_.end() && found->path == path ? &*found : nullptr;
}

std::optional<jsx::Diagnostic> StylesheetSet::resolve(const ModuleSource& source,
                                                      const jsx::ModuleItem& item,
                                                      std::span<const StylesheetFile> files,
                                                      std::vector<CssModuleBinding>& bindings) {
    const std::string path = join_project_path(directory_of(source.file), item.source);
    const auto found = std::find_if(files.begin(), files.end(), [&](const StylesheetFile& sheet) {
        return sheet.path == path;
    });
    if (found == files.end()) {
        return make_diagnostic(source, jsx::Severity::Error, "TESSERA_IR_STYLESHEET_MISSING",
                               "stylesheet `" + item.source + "` (`" + path +
                                   "`) is missing or unreadable",
                               item.span, "create the file or fix the import path");
    }
    // Only a CSS module import carries a class map; a plain `import "./x.css"` is global CSS.
    std::optional<css_module::ModuleScan> scan;
    if (!item.css_module_binding.empty()) {
        std::expected<css_module::ModuleScan, css_module::ModuleScanError> scanned =
            css_module::scan_module_css(path, found->text);
        if (!scanned) {
            return make_diagnostic(source, jsx::Severity::Error,
                                   stable_scan_code(scanned.error().code), scanned.error().message,
                                   item.span, "fix the module file the import names");
        }
        scan = std::move(*scanned);
    }

    const auto existing =
        std::find_if(stylesheets.begin(), stylesheets.end(),
                     [&](const ir::StylesheetRef& sheet) { return sheet.path == path; });
    if (!scan) {
        if (existing == stylesheets.end()) {
            stylesheets.push_back(ir::StylesheetRef{"css::" + without_extension(path), path,
                                                    stylesheet_hash(found->text)});
        }
        return std::nullopt;
    }

    const std::vector<ir::StylesheetClass> classes = to_classes(scan->classes);
    if (existing == stylesheets.end()) {
        stylesheets.push_back(ir::StylesheetRef{"css::" + without_extension(path), path,
                                                stylesheet_hash(found->text), classes});
    } else if (existing->classes.empty()) {
        // The same file imported once for its side effect and once as a module keeps one entry, and
        // the class map is added when the module import is the one that carries it.
        existing->classes = classes;
    }
    bindings.push_back(CssModuleBinding{item.css_module_binding, path, classes});
    // One scanned entry per path: the first import site (files are visited in path order) is the
    // one a warning points at.
    if (find_module(path) == nullptr) {
        modules_.push_back(ScannedModule{path, scan->hash, classes, &source, item.span});
        std::sort(modules_.begin(), modules_.end(),
                  [](const ScannedModule& left, const ScannedModule& right) {
                      return left.path < right.path;
                  });
    }
    return std::nullopt;
}

std::optional<jsx::Diagnostic> StylesheetSet::check_collisions() const {
    // Sorted by path, so the first colliding pair in this walk is deterministic.
    for (std::size_t i = 0; i < modules_.size(); ++i) {
        for (std::size_t j = i + 1; j < modules_.size(); ++j) {
            if (modules_[i].hash != modules_[j].hash) {
                continue;
            }
            return make_diagnostic(
                *modules_[i].source, jsx::Severity::Error, "TESSERA_CSS_MODULE_HASH_COLLISION",
                "CSS modules `" + modules_[i].path + "` and `" + modules_[j].path +
                    "` share the path hash `" + modules_[i].hash + "`",
                modules_[i].span, "rename one of the files so their scoped classes differ");
        }
    }
    return std::nullopt;
}

std::vector<LoweringWarning>
StylesheetSet::unused_classes(std::span<const CssModuleUse> uses) const {
    std::vector<LoweringWarning> warnings;
    for (const ScannedModule& module : modules_) {
        for (const ir::StylesheetClass& declared : module.classes) {
            const bool used = std::any_of(uses.begin(), uses.end(), [&](const CssModuleUse& use) {
                return use.path == module.path && use.local == declared.local;
            });
            if (used) {
                continue;
            }
            warnings.push_back(LoweringWarning{
                std::string(module.source->file),
                make_diagnostic(
                    *module.source, jsx::Severity::Warning, "TESSERA_CSS_MODULE_UNUSED_CLASS",
                    "class `" + declared.local + "` of `" + module.path +
                        "` is never read by a `className` of the project",
                    module.span, "read it through the module binding or delete it")});
        }
    }
    return warnings;
}

} // namespace tessera::compiler::lowering::internal
