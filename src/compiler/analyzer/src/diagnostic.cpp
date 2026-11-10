// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/diagnostic.hpp"

namespace tessera::compiler::jsx {

namespace {

std::string location_text(const Location& location) {
    return std::to_string(location.line) + ":" + std::to_string(location.column);
}

// Cuts `text` to at most `max_bytes` without splitting a UTF-8 code point, so a diagnostic is
// still valid UTF-8 (and diffable) after truncation.
std::string cut_utf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t size = max_bytes;
    // While the first dropped byte is a continuation byte the code point started earlier; step
    // back to the lead byte and drop the whole code point.
    while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) {
        --size;
    }
    std::string out(text.substr(0, size));
    out += "...";
    return out;
}

} // namespace

std::string excerpt(std::string_view text) {
    return cut_utf8(text, kMaxExcerptBytes);
}

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
    // Last line of defense for the bounded-report invariant: no call site can make the collected
    // report grow with the input.
    if (message.size() > kMaxDiagnosticTextBytes) {
        message = cut_utf8(message, kMaxDiagnosticTextBytes);
    }
    if (suggestion.size() > kMaxDiagnosticTextBytes) {
        suggestion = cut_utf8(suggestion, kMaxDiagnosticTextBytes);
    }
    if (related && related->message.size() > kMaxDiagnosticTextBytes) {
        related->message = cut_utf8(related->message, kMaxDiagnosticTextBytes);
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
