// SPDX-License-Identifier: MIT
//
// Range formatting for a .tsx file with recoverable errors (ADR-061 slice 4). The parser recovers
// per top-level item (jsx-subset-v1 section 14): an item that failed is marked invalid and the
// parser skips to the next item start. This pass keeps every such region byte for byte and formats
// the stretches between them as files of their own, so a comment is never lost (a region is
// untouched and a stretch is verified like any file) and the printer never guesses at structure the
// parser could not read.

#include "tsx_format.hpp"

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::format {

namespace {

constexpr std::string_view kSkipped = "TESSERA_FMT_SKIPPED";

bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

struct Range {
    std::size_t begin = 0;
    std::size_t end = 0; // exclusive
};

// One run of output: either a broken region (copied) or a stretch of valid items (formatted).
struct Piece {
    Range range;
    bool broken = false;
};

Range trimmed(std::string_view source, Range r) {
    while (r.begin < r.end && is_blank(source[r.begin])) {
        ++r.begin;
    }
    while (r.end > r.begin && is_blank(source[r.end - 1])) {
        --r.end;
    }
    return r;
}

bool blank_line_between(std::string_view source, std::size_t from, std::size_t to) {
    int newlines = 0;
    for (std::size_t i = from; i < to; ++i) {
        newlines += source[i] == '\n' ? 1 : 0;
    }
    return newlines >= 2;
}

FormatDiagnostic skipped(const jsx::SourceMap& map, std::size_t offset, std::string message) {
    const jsx::Location at = map.locate(static_cast<std::uint32_t>(offset));
    return FormatDiagnostic{std::string(kSkipped), std::move(message), at.line, at.column};
}

} // namespace

FormatResult format_tsx_recovering(const std::string& source, const jsx::SourceMap& map,
                                   const jsx::ModuleParseResult& parsed,
                                   const FormatOptions& options) {
    const std::string_view text = source;
    const std::vector<jsx::ModuleItem>& items = parsed.root->items;

    // A broken region runs from an invalid item's first token to the next item's first token.
    std::vector<Range> raw;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].valid) {
            const std::size_t end = i + 1 < items.size() ? items[i + 1].span.offset : text.size();
            raw.push_back({items[i].span.offset, end});
        }
    }
    const auto refuse_syntax = [&]() {
        FormatResult result;
        std::string message = "the file does not parse, so it is not formatted";
        for (const jsx::Diagnostic& diagnostic : parsed.diagnostics) {
            if (diagnostic.severity == jsx::Severity::Error) {
                message += "\n" + jsx::format_diagnostic(diagnostic);
            }
        }
        const jsx::Location at = map.locate(0);
        result.diagnostics.push_back(
            FormatDiagnostic{"TESSERA_FMT_SYNTAX", std::move(message), at.line, at.column});
        return result;
    };
    // Every error must be inside a region, else the damage is not confined to items the parser gave
    // up on (a file-level error, a limit) and the structure is unknowable.
    if (raw.empty()) {
        return refuse_syntax();
    }
    for (const jsx::Diagnostic& diagnostic : parsed.diagnostics) {
        if (diagnostic.severity != jsx::Severity::Error) {
            continue;
        }
        bool inside = false;
        for (const Range& r : raw) {
            inside = inside || (diagnostic.span.span.offset >= r.begin &&
                                diagnostic.span.span.offset < r.end);
        }
        if (!inside) {
            return refuse_syntax();
        }
    }

    FormatResult result;
    std::vector<Piece> pieces;
    std::size_t cursor = 0;
    for (const Range& r : raw) {
        const Range before = trimmed(text, {cursor, r.begin});
        if (before.begin < before.end) {
            pieces.push_back({before, false});
        }
        const Range region = trimmed(text, r);
        pieces.push_back({region, true});
        result.skipped.push_back(skipped(
            map, region.begin, "left as written, the item does not parse" + [&]() {
                for (const jsx::Diagnostic& d : parsed.diagnostics) {
                    if (d.severity == jsx::Severity::Error && d.span.span.offset >= r.begin &&
                        d.span.span.offset < r.end) {
                        return "\n" + jsx::format_diagnostic(d);
                    }
                }
                return std::string();
            }()));
        cursor = r.end;
    }
    const Range tail = trimmed(text, {cursor, text.size()});
    if (tail.begin < tail.end) {
        pieces.push_back({tail, false});
    }

    // Nothing around the broken items to format: that is a refusal, not a no-op that hides the
    // errors.
    bool any_stretch = false;
    for (const Piece& piece : pieces) {
        any_stretch = any_stretch || !piece.broken;
    }
    if (!any_stretch) {
        return refuse_syntax();
    }

    std::string out;
    std::size_t previous_end = 0;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const Piece& piece = pieces[i];
        std::string body(text.substr(piece.range.begin, piece.range.end - piece.range.begin));
        if (!piece.broken) {
            const std::string stretch = body;
            const jsx::SourceMap stretch_map(stretch);
            FormatResult formatted = format_tsx_text(stretch, stretch_map, options);
            if (formatted.ok()) {
                body = std::move(formatted.text);
                while (!body.empty() && body.back() == '\n') {
                    body.pop_back();
                }
            } else {
                result.skipped.push_back(skipped(map, piece.range.begin,
                                                 "left as written, the printer refused it: " +
                                                     formatted.diagnostics[0].message));
            }
        }
        if (i > 0) {
            out += blank_line_between(text, previous_end, piece.range.begin) ? "\n\n" : "\n";
        }
        out += body;
        previous_end = piece.range.end;
    }
    out += "\n";

    // The regions are verbatim, so the parser must see the same items in the same state.
    const jsx::ModuleParseResult again = jsx::parse_module(out);
    bool same = again.root != nullptr && again.root->items.size() == items.size();
    for (std::size_t i = 0; same && i < items.size(); ++i) {
        same = again.root->items[i].valid == items[i].valid &&
               again.root->items[i].kind == items[i].kind;
    }
    if (!same) {
        FormatResult refused;
        const jsx::Location at = map.locate(0);
        refused.diagnostics.push_back(
            FormatDiagnostic{"TESSERA_FMT_VERIFY",
                             "the printed output splits the items differently from the input; this "
                             "is a formatter bug",
                             at.line, at.column});
        return refused;
    }
    result.text = std::move(out);
    return result;
}

} // namespace tessera::compiler::format
