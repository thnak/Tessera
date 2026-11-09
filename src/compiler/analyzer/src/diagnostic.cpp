// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/diagnostic.hpp"

namespace tessera::compiler::jsx {

namespace {

std::string location_text(const Location& location) {
    return std::to_string(location.line) + ":" + std::to_string(location.column);
}

} // namespace

std::string format_diagnostic(const Diagnostic& diagnostic) {
    std::string out = location_text(diagnostic.span.start) + ": ";
    out += diagnostic.severity == Severity::Error ? "error[" : "warning[";
    out += diagnostic.code;
    out += "]: ";
    out += diagnostic.message;
    if (!diagnostic.suggestion.empty()) {
        out += "\n  help: " + diagnostic.suggestion;
    }
    if (diagnostic.related) {
        out += "\n  note: " + location_text(diagnostic.related->span.start) + ": " +
               diagnostic.related->message;
    }
    return out;
}

void DiagnosticSink::report(std::string_view code, Severity severity, std::string message,
                            Span span, std::string suggestion, std::optional<RelatedSpan> related) {
    if (severity == Severity::Error) {
        ++error_count_;
    }
    if (truncated_) {
        return;
    }
    if (diagnostics_.size() >= limits_.max_diagnostics) {
        truncated_ = true;
        diagnostics_.push_back(
            Diagnostic{"TESSERA_JSX_LIMIT_DIAGNOSTICS",
                       Severity::Error,
                       "too many diagnostics; further diagnostics are not reported",
                       map_->resolve(span),
                       {},
                       std::nullopt});
        return;
    }
    diagnostics_.push_back(Diagnostic{code, severity, std::move(message), map_->resolve(span),
                                      std::move(suggestion), std::move(related)});
}

void DiagnosticSink::error(std::string_view code, std::string message, Span span,
                           std::string suggestion) {
    report(code, Severity::Error, std::move(message), span, std::move(suggestion));
}

void DiagnosticSink::rewind(const Mark& mark) {
    diagnostics_.resize(mark.size, Diagnostic{});
    error_count_ = mark.errors;
    truncated_ = mark.truncated;
}

} // namespace tessera::compiler::jsx
