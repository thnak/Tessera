// SPDX-License-Identifier: MIT
//
// format_source (ADR-061): validate with the parser, print from the token stream, then re-parse
// the result and refuse if it does not mean the same thing or if a comment went missing. The
// verification is what makes the three properties unconditional rather than a matter of testing.

#include "builder.hpp"
#include "other_formats.hpp"
#include "tessera/compiler/format/format.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tsx_format.hpp"

#include <utility>

namespace tessera::compiler::format {

DocId Builder::file() {
    const std::size_t eof = s_.tokens.size() - 1;
    std::vector<DocId> parts;
    parts.push_back(statements(0, eof));
    FTok& end = s_.tokens[eof];
    if (!end.leading.empty()) {
        end.leading_used = true;
        for (std::size_t i = 0; i < end.leading.size(); ++i) {
            if (eof > 0 || i > 0) {
                parts.push_back(a_.hardline());
                if (end.leading[i].blank_before) {
                    parts.push_back(a_.hardline());
                }
            }
            parts.push_back(comment_doc(end.leading[i].id));
        }
    }
    return a_.concat(std::move(parts));
}

namespace {

std::string normalize_newlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string strip_space(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

FormatResult refuse(const jsx::SourceMap& map, std::string code, std::string message,
                    std::uint32_t offset) {
    FormatResult result;
    const jsx::Location at = map.locate(offset);
    result.diagnostics.push_back(
        FormatDiagnostic{std::move(code), std::move(message), at.line, at.column});
    return result;
}

std::string excerpt(std::string_view text) {
    constexpr std::size_t kMax = 40;
    std::string out(text.substr(0, kMax));
    for (char& c : out) {
        if (c == '\n') {
            c = ' ';
        }
    }
    return text.size() > kMax ? out + "..." : out;
}

} // namespace

// The TSX printer proper: `source` is LF-normalised and has no BOM.
FormatResult format_tsx_text(const std::string& source, const jsx::SourceMap& map,
                             const FormatOptions& options) {
    FormatResult result;
    jsx::ModuleParseResult parsed = jsx::parse_module(source);
    if (parsed.root != nullptr && parsed.has_errors() && options.recover) {
        return format_tsx_recovering(source, map, parsed, options);
    }
    if (parsed.root == nullptr || parsed.has_errors()) {
        std::string message = "the file does not parse, so it is not formatted";
        for (const jsx::Diagnostic& diagnostic : parsed.diagnostics) {
            if (diagnostic.severity == jsx::Severity::Error) {
                message += "\n" + jsx::format_diagnostic(diagnostic);
            }
        }
        return refuse(map, "TESSERA_FMT_SYNTAX", std::move(message), 0);
    }

    ScanResult scanned = scan(source);
    if (!scanned.error.empty()) {
        if (scanned.error_is_comment) {
            return refuse(map, "TESSERA_FMT_COMMENT",
                          "a comment cannot be placed: " + scanned.error, scanned.error_offset);
        }
        return refuse(map, "TESSERA_FMT_UNSUPPORTED", "cannot scan the file: " + scanned.error,
                      scanned.error_offset);
    }

    DocArena arena;
    Builder builder(source, scanned, arena);
    const DocId doc = builder.file();
    if (!builder.error().empty()) {
        return refuse(
            map, builder.error_is_comment() ? "TESSERA_FMT_COMMENT" : "TESSERA_FMT_UNSUPPORTED",
            builder.error(), builder.error_offset());
    }
    for (const FTok& tok : scanned.tokens) {
        if (!tok.leading.empty() && !tok.leading_used) {
            const FComment& c = scanned.comments[tok.leading.front().id];
            return refuse(map, "TESSERA_FMT_COMMENT",
                          "comment `" + excerpt(c.text) +
                              "` leads a token that is not the start of a statement, list item or "
                              "attribute, so it cannot be placed; move it to its own statement",
                          c.begin);
        }
    }
    for (const FComment& c : scanned.comments) {
        if (!c.placed) {
            return refuse(map, "TESSERA_FMT_COMMENT",
                          "comment `" + excerpt(c.text) + "` cannot be placed", c.begin);
        }
    }

    std::string formatted = print_doc(arena, doc, options.print_width, options.indent_width);
    if (scanned.tokens.size() == 1 && scanned.comments.empty()) {
        formatted.clear(); // an empty file stays empty
    }

    // Verification: same meaning, same comments.
    jsx::ModuleParseResult again = jsx::parse_module(formatted);
    if (again.root == nullptr || again.has_errors()) {
        return refuse(map, "TESSERA_FMT_VERIFY",
                      "the printed output does not parse; this is a formatter bug", 0);
    }
    if (jsx::dump(*parsed.root) != jsx::dump(*again.root)) {
        return refuse(
            map, "TESSERA_FMT_VERIFY",
            "the printed output does not mean the same as the input; this is a formatter bug", 0);
    }
    bool same_comments = parsed.comments.size() == again.comments.size();
    for (std::size_t i = 0; same_comments && i < parsed.comments.size(); ++i) {
        same_comments = parsed.comments[i].block == again.comments[i].block &&
                        strip_space(parsed.comments[i].text) == strip_space(again.comments[i].text);
    }
    if (!same_comments) {
        return refuse(map, "TESSERA_FMT_VERIFY",
                      "the printed output changed the comments; this is a formatter bug", 0);
    }

    result.text = std::move(formatted);
    return result;
}

namespace {

FormatResult format_plain(FileKind kind, const std::string& source, const jsx::SourceMap& map,
                          const FormatOptions& options) {
    Problem problem;
    std::string out;
    const bool ok = kind == FileKind::Css ? format_css(source, options, out, problem)
                                          : format_json(source, options, out, problem);
    if (!ok) {
        return refuse(map, std::move(problem.code), std::move(problem.message),
                      static_cast<std::uint32_t>(problem.offset));
    }
    FormatResult result;
    result.text = std::move(out);
    return result;
}

} // namespace

FormatResult format_source(FileKind kind, std::string_view text, const FormatOptions& options) {
    std::string_view body = text;
    const bool bom = body.starts_with("\xEF\xBB\xBF");
    if (bom) {
        body.remove_prefix(3);
    }
    const bool crlf = body.find("\r\n") != std::string_view::npos;
    const std::string source = normalize_newlines(body);
    const jsx::SourceMap map(source);

    FormatResult inner = kind == FileKind::Tsx ? format_tsx_text(source, map, options)
                                               : format_plain(kind, source, map, options);
    if (!inner.ok()) {
        return inner;
    }
    FormatResult result;
    result.skipped = std::move(inner.skipped);
    std::string formatted = std::move(inner.text);
    if (crlf) {
        std::string converted;
        for (const char c : formatted) {
            if (c == '\n') {
                converted.push_back('\r');
            }
            converted.push_back(c);
        }
        formatted = std::move(converted);
    }
    if (bom) {
        formatted.insert(0, "\xEF\xBB\xBF");
    }
    result.text = std::move(formatted);
    result.changed = result.text != text;
    return result;
}

std::optional<FileKind> file_kind_for_path(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (name == "tessera.json") {
        return FileKind::Json;
    }
    if (name.ends_with(".tsx")) {
        return FileKind::Tsx;
    }
    if (name.ends_with(".css")) {
        return FileKind::Css;
    }
    return std::nullopt;
}

} // namespace tessera::compiler::format
