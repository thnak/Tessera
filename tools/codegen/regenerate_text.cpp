// SPDX-License-Identifier: MIT

#include "regenerate_text.hpp"

#include <algorithm>
#include <filesystem>

namespace tessera::compiler::codegen::cli::detail {

namespace fs = std::filesystem;

std::string diagnostic_text(std::string_view path, const jsx::Diagnostic& diagnostic) {
    return std::string(path) + ":" + jsx::format_diagnostic(diagnostic);
}

std::string ir_diagnostic_text(const ir::IrDiagnostic& diagnostic) {
    const ir::SourceSpan& span = diagnostic.span;
    std::string text;
    if (!span.file.empty()) {
        text +=
            span.file + ":" + std::to_string(span.line) + ":" + std::to_string(span.column) + ": ";
    }
    return text + diagnostic.code + ": " + diagnostic.message;
}

bool safe_file_name(std::string_view name) {
    return !name.empty() && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && name.find("..") == std::string_view::npos;
}

// A module namespace when --namespace is absent: the file stem with non-identifier characters
// replaced, leading digits prefixed, and lowercased (C++ namespace convention).
std::string derive_module_name(const std::string& input) {
    const fs::path path(input);
    std::string stem = path.stem().string();
    std::string name;
    for (const char c : stem) {
        const auto ch = static_cast<unsigned char>(c);
        name += ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                 c == '_')
                    ? static_cast<char>(ch >= 'A' && ch <= 'Z' ? c - 'A' + 'a' : c)
                    : '_';
    }
    if (name.empty()) {
        name = "module";
    }
    if (name.front() >= '0' && name.front() <= '9') {
        name.insert(name.begin(), '_');
    }
    return name;
}

} // namespace tessera::compiler::codegen::cli::detail
