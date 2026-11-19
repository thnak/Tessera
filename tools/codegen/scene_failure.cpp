// SPDX-License-Identifier: MIT

#include "diagnose.hpp"
#include "scene_failure.hpp"

#include <string>
#include <vector>

namespace tessera::compiler::codegen::cli {

std::string format_record_line(const DiagnosticRecord& record, std::string_view fallback_file) {
    std::string line;
    if (record.span.has_location()) {
        line += record.span.file + ":" + std::to_string(record.span.line) + ":" +
                std::to_string(record.span.column) + ": ";
    } else {
        line += std::string(fallback_file) + ": ";
    }
    line += record.severity + " ";
    if (!record.code.empty()) {
        line += record.code + ": ";
    }
    line += record.message;
    if (!record.suggestion.empty()) {
        line += " (hint: " + record.suggestion + ")";
    }
    return line;
}

std::string scene_failure_text(std::string_view source, std::string_view file,
                               std::string_view scene_error) {
    std::string text;
    for (const DiagnosticRecord& record : collect_source_diagnostics(source, file)) {
        text += format_record_line(record, file) + "\n";
    }
    if (text.empty()) {
        text = std::string(file) + ": error: " +
               std::string(scene_error.empty() ? "the source produced no scene" : scene_error) +
               "\n";
    }
    return text;
}

} // namespace tessera::compiler::codegen::cli
