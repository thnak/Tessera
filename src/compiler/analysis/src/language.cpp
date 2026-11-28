// SPDX-License-Identifier: MIT

#include "tessera/compiler/analysis/language.hpp"
#include "tessera/compiler/analysis/symbol_json.hpp"
#include "utf16_map.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::compiler::analysis {

namespace {

constexpr std::size_t kMaxSignature = 200;

// What a caret names: the declaration the name stands for, or why there is none.
struct Target {
    Span range;
    const Symbol* symbol = nullptr; // the declaration, imports followed
    const Reference* reference = nullptr;
    Unresolved reason = Unresolved::None;
    std::string kind; // for a name with no declaration
    std::string name;
};

std::optional<Target> target_at(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    const auto located = index.locate(file, offset);
    if (!located) {
        return std::nullopt;
    }
    Target target;
    if (located->declaration) {
        target.symbol = index.declaration_of(located->symbol);
        const Symbol* own = index.symbol(located->symbol);
        target.range = own->span;
        target.name = own->name;
        return target;
    }
    const Reference& reference = *located->reference;
    target.range = reference.span;
    target.name = reference.name;
    target.kind = std::string(wire_name(reference.kind));
    target.reference = &reference;
    if (reference.resolution.resolved()) {
        target.symbol = index.declaration_of(reference.resolution.symbol);
    } else {
        target.reason = reference.resolution.reason;
    }
    return target;
}

// Replaces bytes that are not valid UTF-8 with U+FFFD, so the JSON text is always valid.
std::string valid_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0x80           ? 1
                             : (lead >> 5) == 0x6  ? 2
                             : (lead >> 4) == 0xE  ? 3
                             : (lead >> 3) == 0x1E ? 4
                                                   : 0;
        bool ok = length != 0 && i + length <= text.size();
        for (std::size_t k = 1; ok && k < length; ++k) {
            ok = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        }
        if (ok) {
            out.append(text.substr(i, length));
            i += length;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

struct Lines {
    std::string_view text;

    [[nodiscard]] std::size_t start_of(std::size_t byte) const {
        const auto at = text.rfind('\n', byte == 0 ? 0 : byte - 1);
        return byte == 0 || at == std::string_view::npos ? 0 : at + 1;
    }
    [[nodiscard]] std::string_view line_at(std::size_t start) const {
        const auto end = text.find('\n', start);
        return text.substr(start,
                           end == std::string_view::npos ? text.size() - start : end - start);
    }
};

std::string_view strip_comment_marks(std::string_view line) {
    line = trim(line);
    for (const std::string_view mark : {"/**", "/*", "*/", "///", "//", "*"}) {
        if (line.starts_with(mark)) {
            line.remove_prefix(mark.size());
            break;
        }
    }
    if (line.ends_with("*/")) {
        line.remove_suffix(2);
    }
    return trim(line);
}

// The contiguous comment lines directly above the line starting at `start`.
std::string doc_above(const Lines& lines, std::size_t start) {
    std::vector<std::string_view> collected;
    std::size_t cursor = start;
    bool in_block = false;
    while (cursor > 0) {
        const std::size_t prev_start = lines.start_of(cursor - 1);
        const std::string_view raw = trim(lines.line_at(prev_start));
        if (in_block) {
            collected.push_back(strip_comment_marks(raw));
            in_block = raw.find("/*") == std::string_view::npos;
        } else if (raw.starts_with("//")) {
            collected.push_back(strip_comment_marks(raw));
        } else if (raw.ends_with("*/")) {
            collected.push_back(strip_comment_marks(raw));
            in_block = raw.find("/*") == std::string_view::npos;
        } else {
            break;
        }
        cursor = prev_start;
    }
    if (in_block) {
        return {}; // an unterminated block above: not a doc comment
    }
    std::ranges::reverse(collected);
    while (!collected.empty() && collected.front().empty()) {
        collected.erase(collected.begin());
    }
    while (!collected.empty() && collected.back().empty()) {
        collected.pop_back();
    }
    std::string out;
    for (const std::string_view line : collected) {
        out += out.empty() ? "" : "\n";
        out += line;
    }
    return out;
}

std::string signature_of(std::string_view line, SymbolKind kind) {
    line = trim(line);
    if (kind == SymbolKind::Component || kind == SymbolKind::Hook || kind == SymbolKind::Helper ||
        kind == SymbolKind::Handler) {
        int depth = 0;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c == '(' || c == '[') {
                ++depth;
            } else if (c == ')' || c == ']') {
                --depth;
            } else if (c == '{' && depth <= 0) {
                line = trim(line.substr(0, i));
                break;
            }
        }
    }
    std::string out = valid_utf8(line);
    if (out.size() > kMaxSignature) {
        std::size_t cut = kMaxSignature;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
        out += "...";
    }
    return out;
}

bool has_statement(SymbolKind kind) {
    return kind != SymbolKind::Prop && kind != SymbolKind::PropsParam && kind != SymbolKind::Param;
}

void span_json(std::string& out, const Span& span) {
    out += "\"file\":" + std::to_string(span.file) + ",\"from\":" + std::to_string(span.from) +
           ",\"to\":" + std::to_string(span.to);
}

void member(std::string& out, std::string_view key, std::string_view value) {
    out += ",\"";
    out += key;
    out += "\":";
    append_json_string(out, value);
}

} // namespace

Definition definition(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    Definition out;
    const auto target = target_at(index, file, offset);
    if (!target) {
        return out;
    }
    if (target->symbol != nullptr) {
        out.spans.push_back(target->symbol->span);
    } else {
        out.reason = target->reason;
    }
    return out;
}

ReferenceList find_references(const ProjectIndex& index, FileId file, std::uint32_t offset,
                              bool include_declaration) {
    ReferenceList out;
    const auto target = target_at(index, file, offset);
    if (!target) {
        return out;
    }
    if (target->symbol == nullptr) {
        out.reason = target->reason;
        out.name = target->name;
        return out;
    }
    out.symbol = target->symbol->id;
    out.name = target->symbol->name;
    out.occurrences = index.references_to(out.symbol, include_declaration);
    return out;
}

std::optional<Hover> hover(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    Hover out;
    const auto target = target_at(index, file, offset);
    // A diagnostic covers the caret inside its span; an empty span covers only its own offset.
    for (const Diagnostic& d : index.diagnostics(file)) {
        const bool covers = d.span.to == d.span.from ? offset == d.span.from
                                                     : d.span.from <= offset && offset < d.span.to;
        if (covers) {
            out.diagnostics.push_back(d);
        }
    }
    if (!target) {
        if (out.diagnostics.empty()) {
            return std::nullopt;
        }
        out.range = out.diagnostics.front().span;
        return out;
    }
    out.range = Span{file, target->range.from, target->range.to};
    out.name = target->name;
    out.reason = target->reason;
    if (target->symbol == nullptr) {
        out.kind = target->kind;
        return out;
    }
    const Symbol& symbol = *target->symbol;
    out.resolved = true;
    out.kind = std::string(wire_name(symbol.kind));
    out.name = symbol.name;
    out.exported = symbol.exported;
    out.declaration = symbol.span;
    if (const Symbol* parent = index.symbol(symbol.parent)) {
        out.container = parent->name;
    }
    if (has_statement(symbol.kind)) {
        if (const auto text = index.text(symbol.span.file)) {
            const Utf16Map map(*text);
            const Lines lines{*text};
            const std::size_t start = lines.start_of(map.to_byte(symbol.span.from));
            out.signature = signature_of(lines.line_at(start), symbol.kind);
            out.doc = valid_utf8(doc_above(lines, start));
        }
    }
    return out;
}

std::string definition_to_json(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    const Definition result = definition(index, file, offset);
    std::string out = "{\"definitions\":[";
    for (std::size_t i = 0; i < result.spans.size(); ++i) {
        out += i == 0 ? "{" : ",{";
        span_json(out, result.spans[i]);
        out += '}';
    }
    out += ']';
    if (result.reason != Unresolved::None) {
        member(out, "unresolved", wire_name(result.reason));
    }
    out += '}';
    return out;
}

std::string references_to_json(const ProjectIndex& index, FileId file, std::uint32_t offset,
                               bool include_declaration) {
    const ReferenceList result = find_references(index, file, offset, include_declaration);
    std::string out = "{\"symbol\":";
    if (result.symbol.empty()) {
        out += "null";
    } else {
        append_json_string(out, result.symbol);
        member(out, "name", result.name);
    }
    out += ",\"references\":[";
    for (std::size_t i = 0; i < result.occurrences.size(); ++i) {
        const Occurrence& o = result.occurrences[i];
        out += i == 0 ? "{" : ",{";
        span_json(out, o.span);
        member(out, "kind", o.declaration ? "declaration" : wire_name(o.kind));
        out += '}';
    }
    out += ']';
    if (result.reason != Unresolved::None) {
        member(out, "unresolved", wire_name(result.reason));
    }
    out += '}';
    return out;
}

std::string hover_to_json(const ProjectIndex& index, FileId file, std::uint32_t offset) {
    const auto result = hover(index, file, offset);
    if (!result) {
        return "{\"hover\":null}";
    }
    std::string out = "{\"hover\":{\"from\":" + std::to_string(result->range.from) +
                      ",\"to\":" + std::to_string(result->range.to);
    member(out, "kind", result->kind);
    member(out, "name", result->name);
    if (result->resolved) {
        member(out, "signature", result->signature);
        member(out, "doc", result->doc);
        member(out, "container", result->container);
        out += std::string(",\"exported\":") + (result->exported ? "true" : "false");
        out += ",\"declaration\":{";
        span_json(out, *result->declaration);
        out += '}';
    } else if (result->reason != Unresolved::None) {
        member(out, "unresolved", wire_name(result->reason));
    }
    out += ",\"diagnostics\":[";
    for (std::size_t i = 0; i < result->diagnostics.size(); ++i) {
        const Diagnostic& d = result->diagnostics[i];
        out += i == 0 ? "{" : ",{";
        out += "\"code\":";
        append_json_string(out, d.code);
        out += d.severity == DiagnosticSeverity::Error ? ",\"severity\":\"error\""
                                                       : ",\"severity\":\"warning\"";
        member(out, "message", d.message);
        out += ",\"from\":" + std::to_string(d.span.from) + ",\"to\":" + std::to_string(d.span.to);
        out += '}';
    }
    out += "]}}";
    return out;
}

} // namespace tessera::compiler::analysis
