// SPDX-License-Identifier: MIT

#include "diagnostic_record.hpp"

#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {

namespace {

DiagnosticSpan to_span(const jsx::ResolvedSpan& span, std::string_view file) {
    DiagnosticSpan out;
    out.file = std::string(file);
    out.offset = span.span.offset;
    out.length = span.span.length;
    out.line = span.start.line;
    out.column = span.start.column;
    out.end_line = span.end.line;
    out.end_column = span.end.column;
    return out;
}

DiagnosticSpan to_span(const ir::SourceSpan& span) {
    DiagnosticSpan out;
    out.file = span.file;
    out.offset = span.offset;
    out.length = span.length;
    out.line = span.line;
    out.column = span.column;
    out.end_line = span.end_line;
    out.end_column = span.end_column;
    return out;
}

} // namespace

DiagnosticRecord to_record(const jsx::Diagnostic& diagnostic, std::string_view file) {
    DiagnosticRecord record;
    record.code = std::string(diagnostic.code);
    // The document uses the spec's spelling (jsx-subset-v1.md section 13.1), not the enum name.
    record.severity = diagnostic.severity == jsx::Severity::Error ? "error" : "warning";
    record.message = diagnostic.message;
    record.span = to_span(diagnostic.span, file);
    record.suggestion = diagnostic.suggestion;
    if (diagnostic.related) {
        record.related.push_back(DiagnosticRelated{to_span(diagnostic.related->span, file),
                                                   diagnostic.related->message});
    }
    return record;
}

DiagnosticRecord to_record(const ir::IrDiagnostic& diagnostic) {
    DiagnosticRecord record;
    record.code = diagnostic.code;
    // `ir::validate` reports hard errors only (validate.hpp), so the severity is always `error`.
    record.severity = "error";
    record.message = diagnostic.message;
    record.span = to_span(diagnostic.span);
    return record;
}

} // namespace tessera::compiler::codegen::cli
