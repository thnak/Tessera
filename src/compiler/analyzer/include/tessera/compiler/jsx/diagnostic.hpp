// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/limits.hpp"
#include "tessera/compiler/jsx/source_map.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::jsx {

enum class Severity { Error, Warning };

// A second location that explains the first, for example the opening tag of a mismatched
// closing tag.
struct RelatedSpan {
    ResolvedSpan span;
    std::string message;
};

// docs/specs/jsx-subset-v1.md section 13.1. `code` points at a string literal and is stable.
struct Diagnostic {
    std::string_view code;
    Severity severity = Severity::Error;
    std::string message;
    ResolvedSpan span;
    std::string suggestion;
    std::optional<RelatedSpan> related;
};

// "line:column: error[CODE]: message" plus optional help/note lines, for goldens and the CLI.
std::string format_diagnostic(const Diagnostic& diagnostic);

// Collects diagnostics in report order and enforces Limits::max_diagnostics: the first N are
// kept, then one TESSERA_JSX_LIMIT_DIAGNOSTICS entry, and the rest are only counted.
class DiagnosticSink {
public:
    // Snapshot for speculative parsing; rewind() drops everything reported after it.
    struct Mark {
        std::size_t size = 0;
        std::size_t errors = 0;
        bool truncated = false;
    };

    DiagnosticSink(const SourceMap& map, const Limits& limits)
        : map_(&map),
          limits_(limits) {}

    void report(std::string_view code, Severity severity, std::string message, Span span,
                std::string suggestion = {}, std::optional<RelatedSpan> related = std::nullopt);
    void error(std::string_view code, std::string message, Span span, std::string suggestion = {});

    [[nodiscard]] Mark mark() const {
        return Mark{diagnostics_.size(), error_count_, truncated_};
    }
    void rewind(const Mark& mark);

    [[nodiscard]] const SourceMap& map() const {
        return *map_;
    }
    [[nodiscard]] std::size_t error_count() const {
        return error_count_;
    }
    [[nodiscard]] bool has_errors() const {
        return error_count_ > 0;
    }
    [[nodiscard]] bool truncated() const {
        return truncated_;
    }
    [[nodiscard]] std::vector<Diagnostic> take() {
        return std::move(diagnostics_);
    }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

private:
    const SourceMap* map_;
    Limits limits_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t error_count_ = 0;
    bool truncated_ = false;
};

} // namespace tessera::compiler::jsx
