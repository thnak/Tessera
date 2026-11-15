// SPDX-License-Identifier: MIT

// The one writer of the machine-readable diagnostics document
// (docs/specs/compiler-diagnostics-v1.md).
//
// There is exactly one assembly path: `tessera_codegen --diagnostics-json` writes this document and
// the browser module (`tools/codegen/wasm_entry.cpp`) returns it, so the native-versus-Wasm
// diagnostics parity gate compares one implementation against itself rather than two that happen to
// agree (scripts/ci/wasm_diagnostics_parity.py).
//
// The document is written with the compiler's canonical JSON writer (`ir::to_text`), which sorts
// object keys by byte and never depends on construction order, so the text is a pure function of
// the records. Diagnostic *text* is not re-truncated here: the front end's DiagnosticSink already
// bounds every message, suggestion and related message at `jsx::kMaxDiagnosticTextBytes`
// (docs/specs/jsx-subset-v1-jsx.md section 15). A second cut would be a second rule.

#include "diagnostics_json.hpp"
#include "tessera/compiler/ir/json_value.hpp"

#include <fstream>
#include <utility>

namespace tessera::compiler::codegen::cli {

namespace {

// docs/specs/designer-protocol-v1.md section 3.1: a span's `file` is a project-relative path with
// POSIX separators. The CLI is handed host-native paths, so the document normalizes them here, at
// the one place that writes the shape: the page can then match a span to a project file by string,
// and a native run and a Wasm run of the same invocation produce the same document.
std::string posix_path(std::string_view path) {
    std::string out(path);
    for (char& c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

ir::JsonValue span_to_json(const DiagnosticSpan& span) {
    ir::JsonValue json = ir::JsonValue::object();
    json.set("file", ir::JsonValue::string(posix_path(span.file)));
    json.set("offset", ir::JsonValue::integer(span.offset));
    json.set("length", ir::JsonValue::integer(span.length));
    json.set("line", ir::JsonValue::integer(span.line));
    json.set("column", ir::JsonValue::integer(span.column));
    json.set("endLine", ir::JsonValue::integer(span.end_line));
    json.set("endColumn", ir::JsonValue::integer(span.end_column));
    return json;
}

ir::JsonValue record_to_json(const DiagnosticRecord& record) {
    ir::JsonValue json = ir::JsonValue::object();
    json.set("code", ir::JsonValue::string(record.code));
    json.set("severity", ir::JsonValue::string(record.severity));
    json.set("message", ir::JsonValue::string(record.message));
    // A record without a source location (a setup or usage error) has no `span` member at all: an
    // invented 1:1 would be a false location and the page would underline the first character.
    if (record.span.has_location()) {
        json.set("span", span_to_json(record.span));
    }
    if (!record.suggestion.empty()) {
        json.set("suggestion", ir::JsonValue::string(record.suggestion));
    }
    if (!record.related.empty()) {
        ir::JsonValue related = ir::JsonValue::array();
        for (const DiagnosticRelated& entry : record.related) {
            ir::JsonValue item = ir::JsonValue::object();
            item.set("span", span_to_json(entry.span));
            item.set("message", ir::JsonValue::string(entry.message));
            related.push(std::move(item));
        }
        json.set("related", std::move(related));
    }
    return json;
}

} // namespace

std::string write_diagnostics_json(std::string_view primary_file,
                                   const std::vector<DiagnosticRecord>& diagnostics) {
    ir::JsonValue document = ir::JsonValue::object();
    document.set("diagnosticsVersion", ir::JsonValue::integer(kDiagnosticsVersion));
    document.set("file", ir::JsonValue::string(posix_path(primary_file)));
    ir::JsonValue list = ir::JsonValue::array();
    for (const DiagnosticRecord& record : diagnostics) {
        list.push(record_to_json(record));
    }
    document.set("diagnostics", std::move(list));
    return ir::to_text(document);
}

bool write_diagnostics_file(const std::string& path, std::string_view primary_file,
                            const std::vector<DiagnosticRecord>& diagnostics, std::string& error) {
    const std::string text = write_diagnostics_json(primary_file, diagnostics);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write `" + path + "`";
        return false;
    }
    out << text;
    if (!out) {
        error = "cannot write `" + path + "`";
        return false;
    }
    return true;
}

} // namespace tessera::compiler::codegen::cli
