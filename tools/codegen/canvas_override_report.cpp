// SPDX-License-Identifier: MIT

// The override operations' reports (issue #536): what the page parses after `style.override.*`,
// `edit.undo`/`edit.redo` and a commit's settle. Split from `canvas_report.cpp`, which is at the
// source-size limit; the string encoder is the one that file and the stylesheet diagnostics already
// share (`quoted()`), so a quote in a value cannot break the document.

#include "canvas_report.hpp"
#include "css_diagnostics_report.hpp"

#include <string>

namespace tessera::compiler::codegen::cli {
namespace {

void append_flag(std::string& out, std::string_view name, bool value) {
    out += ",\"";
    out += name;
    out += "\":";
    out += value ? "true" : "false";
}

void append_diagnostics(std::string& out, const std::vector<OverrideDiagnostic>& diagnostics) {
    if (diagnostics.empty()) {
        return;
    }
    out += ",\"diagnostics\":[";
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        if (index != 0) {
            out.push_back(',');
        }
        out += "{\"code\":";
        out += quoted(diagnostics[index].code);
        out += ",\"message\":";
        out += quoted(diagnostics[index].message);
        out.push_back('}');
    }
    out.push_back(']');
}

void append_overrides(std::string& out, const std::vector<StyleOverrideInfo>& overrides) {
    out += ",\"overrides\":[";
    for (std::size_t index = 0; index < overrides.size(); ++index) {
        if (index != 0) {
            out.push_back(',');
        }
        out += "{\"node\":\"";
        out += std::to_string(overrides[index].automation_id);
        out += "\",\"property\":";
        out += quoted(overrides[index].property);
        out += ",\"value\":";
        out += quoted(overrides[index].value);
        out.push_back('}');
    }
    out.push_back(']');
}

} // namespace

std::string canvas_override_report_json(const OverrideOutcome& outcome) {
    std::string out = "{\"overrideVersion\":1";
    append_flag(out, "ok", outcome.ok);
    if (!outcome.reason.empty()) {
        out += ",\"reason\":";
        out += quoted(outcome.reason);
    }
    append_flag(out, "changed", outcome.changed);
    append_flag(out, "canUndo", outcome.can_undo);
    append_flag(out, "canRedo", outcome.can_redo);
    append_diagnostics(out, outcome.diagnostics);
    append_overrides(out, outcome.overrides);
    out += "}\n";
    return out;
}

std::string canvas_settle_report_json(const CommitSettleResult& result) {
    std::string out = "{\"settleVersion\":1";
    append_flag(out, "ok", result.ok);
    if (!result.reason.empty()) {
        out += ",\"reason\":";
        out += quoted(result.reason);
    }
    append_flag(out, "consumed", result.consumed);
    append_diagnostics(out, result.diagnostics);
    append_overrides(out, result.overrides);
    out += "}\n";
    return out;
}

} // namespace tessera::compiler::codegen::cli
