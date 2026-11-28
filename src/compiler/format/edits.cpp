// SPDX-License-Identifier: MIT
//
// The edit-list output of the formatter (ADR-061 section 3): the printed text is diffed against the
// input over small atoms, so a format changes only the bytes it must and the editor keeps its
// caret, selection, undo history and diagnostic spans everywhere else.

#include "tessera/compiler/format/edits.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace tessera::compiler::format {

namespace {

struct Atom {
    std::size_t begin = 0;
    std::size_t end = 0;
};

bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_word(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '$' || c >= 0x80;
}

// Whitespace runs, word runs (non-ASCII bytes included, so a code point is never split) and single
// punctuation bytes. Splitting punctuation lets `'a'` -> `"a"` touch only the two quotes.
std::vector<Atom> atoms(std::string_view text, std::size_t from, std::size_t to) {
    std::vector<Atom> out;
    std::size_t i = from;
    while (i < to) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t j = i + 1;
        if (is_space(c)) {
            while (j < to && is_space(static_cast<unsigned char>(text[j]))) {
                ++j;
            }
        } else if (is_word(c)) {
            while (j < to && is_word(static_cast<unsigned char>(text[j]))) {
                ++j;
            }
        }
        out.push_back({i, j});
        i = j;
    }
    return out;
}

// Myers' O(ND) shortest edit script over atoms. Returns false when the script is longer than
// `kMaxEdits`, and the caller falls back to one edit over the whole changed region.
constexpr int kMaxEdits = 1500;

struct Diff {
    std::string_view a_text;
    std::string_view b_text;
    const std::vector<Atom>& a;
    const std::vector<Atom>& b;

    bool same(std::size_t i, std::size_t j) const {
        return a_text.substr(a[i].begin, a[i].end - a[i].begin) ==
               b_text.substr(b[j].begin, b[j].end - b[j].begin);
    }
};

// `keep[i]` is true for an atom of a that is matched to an atom of b; `match_b[j]` likewise.
bool myers(const Diff& d, std::vector<char>& keep_a, std::vector<char>& keep_b) {
    const int n = static_cast<int>(d.a.size());
    const int m = static_cast<int>(d.b.size());
    const int max = std::min(n + m, kMaxEdits);
    const int offset = max + 1;
    std::vector<int> v(static_cast<std::size_t>(2 * max + 3), 0);
    std::vector<std::vector<int>> trace;
    int found = -1;
    for (int cost = 0; cost <= max && found < 0; ++cost) {
        trace.push_back(v);
        for (int k = -cost; k <= cost; k += 2) {
            int x = 0;
            if (k == -cost || (k != cost && v[offset + k - 1] < v[offset + k + 1])) {
                x = v[offset + k + 1];
            } else {
                x = v[offset + k - 1] + 1;
            }
            int y = x - k;
            while (x < n && y < m &&
                   d.same(static_cast<std::size_t>(x), static_cast<std::size_t>(y))) {
                ++x;
                ++y;
            }
            v[offset + k] = x;
            if (x >= n && y >= m) {
                found = cost;
                break;
            }
        }
    }
    if (found < 0) {
        return false;
    }
    int x = n;
    int y = m;
    for (int cost = found; cost > 0; --cost) {
        const std::vector<int>& pv = trace[static_cast<std::size_t>(cost)];
        const int k = x - y;
        int prev_k = 0;
        if (k == -cost || (k != cost && pv[offset + k - 1] < pv[offset + k + 1])) {
            prev_k = k + 1;
        } else {
            prev_k = k - 1;
        }
        const int prev_x = pv[offset + prev_k];
        const int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            --x;
            --y;
            keep_a[static_cast<std::size_t>(x)] = 1;
            keep_b[static_cast<std::size_t>(y)] = 1;
        }
        x = prev_x;
        y = prev_y;
    }
    while (x > 0 && y > 0) { // the cost-0 snake
        --x;
        --y;
        keep_a[static_cast<std::size_t>(x)] = 1;
        keep_b[static_cast<std::size_t>(y)] = 1;
    }
    return true;
}

// UTF-16 length of the UTF-8 bytes [from, to).
std::uint32_t utf16_units(std::string_view text, std::size_t from, std::size_t to) {
    std::uint32_t units = 0;
    for (std::size_t i = from; i < to; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c & 0xC0U) == 0x80U) {
            continue; // a continuation byte belongs to the lead byte already counted
        }
        units += c >= 0xF0U ? 2U : 1U;
    }
    return units;
}

struct ByteEdit {
    std::size_t a_begin;
    std::size_t a_end;
    std::size_t b_begin;
    std::size_t b_end;
};

} // namespace

std::vector<TextEdit> diff_text_edits(std::string_view before, std::string_view after) {
    // Trim the common prefix and suffix on code point boundaries; most edits live in the middle.
    std::size_t prefix = 0;
    const std::size_t limit = std::min(before.size(), after.size());
    while (prefix < limit && before[prefix] == after[prefix]) {
        ++prefix;
    }
    while (prefix > 0 && prefix < before.size() &&
           (static_cast<unsigned char>(before[prefix]) & 0xC0U) == 0x80U) {
        --prefix;
    }
    std::size_t suffix = 0;
    while (suffix < limit - prefix &&
           before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
        ++suffix;
    }
    while (suffix > 0 && before.size() - suffix < before.size() &&
           (static_cast<unsigned char>(before[before.size() - suffix]) & 0xC0U) == 0x80U) {
        --suffix;
    }
    const std::size_t a_end = before.size() - suffix;
    const std::size_t b_end = after.size() - suffix;
    if (prefix == a_end && prefix == b_end) {
        return {};
    }

    const std::vector<Atom> a = atoms(before, prefix, a_end);
    const std::vector<Atom> b = atoms(after, prefix, b_end);
    std::vector<char> keep_a(a.size(), 0);
    std::vector<char> keep_b(b.size(), 0);
    const Diff d{before, after, a, b};
    std::vector<ByteEdit> hunks;
    if (!myers(d, keep_a, keep_b)) {
        hunks.push_back({prefix, a_end, prefix, b_end});
    } else {
        std::size_t i = 0;
        std::size_t j = 0;
        std::size_t a_pos = prefix;
        std::size_t b_pos = prefix;
        while (i < a.size() || j < b.size()) {
            if (i < a.size() && j < b.size() && keep_a[i] != 0 && keep_b[j] != 0) {
                a_pos = a[i].end;
                b_pos = b[j].end;
                ++i;
                ++j;
                continue;
            }
            ByteEdit hunk{a_pos, a_pos, b_pos, b_pos};
            while (i < a.size() && keep_a[i] == 0) {
                hunk.a_end = a[i].end;
                ++i;
            }
            while (j < b.size() && keep_b[j] == 0) {
                hunk.b_end = b[j].end;
                ++j;
            }
            a_pos = hunk.a_end;
            b_pos = hunk.b_end;
            hunks.push_back(hunk);
        }
    }

    std::vector<TextEdit> edits;
    std::size_t cursor_byte = 0;
    std::uint32_t cursor_units = 0;
    for (const ByteEdit& h : hunks) {
        cursor_units += utf16_units(before, cursor_byte, h.a_begin);
        cursor_byte = h.a_begin;
        TextEdit edit;
        edit.offset = cursor_units;
        edit.length = utf16_units(before, h.a_begin, h.a_end);
        edit.new_text = std::string(after.substr(h.b_begin, h.b_end - h.b_begin));
        edits.push_back(std::move(edit));
    }
    return edits;
}

FormatEditsResult format_edits(FileKind kind, std::string_view text, const FormatOptions& options) {
    FormatEditsResult out;
    FormatResult formatted = format_source(kind, text, options);
    out.skipped = std::move(formatted.skipped);
    if (!formatted.ok()) {
        out.diagnostics = std::move(formatted.diagnostics);
        return out;
    }
    if (formatted.changed) {
        out.edits = diff_text_edits(text, formatted.text);
    }
    return out;
}

namespace {

void append_json_string(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(ch);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20U) {
            out += "\\u00";
            out.push_back(kHex[c >> 4U]);
            out.push_back(kHex[c & 0xFU]);
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
}

void append_diagnostics(std::string& out, const std::vector<FormatDiagnostic>& list) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        const FormatDiagnostic& d = list[i];
        out += i == 0 ? "" : ",";
        out += "{\"code\":";
        append_json_string(out, d.code);
        out += ",\"message\":";
        append_json_string(out, d.message);
        out +=
            ",\"line\":" + std::to_string(d.line) + ",\"column\":" + std::to_string(d.column) + "}";
    }
}

} // namespace

std::string format_edits_json(FileKind kind, std::string_view text, const FormatOptions& options) {
    const FormatEditsResult result = format_edits(kind, text, options);
    std::string out = "{\"edits\":[";
    for (std::size_t i = 0; i < result.edits.size(); ++i) {
        const TextEdit& e = result.edits[i];
        out += i == 0 ? "" : ",";
        out += "{\"offset\":" + std::to_string(e.offset) +
               ",\"length\":" + std::to_string(e.length) + ",\"newText\":";
        append_json_string(out, e.new_text);
        out += "}";
    }
    out += "],\"diagnostics\":[";
    append_diagnostics(out, result.diagnostics);
    out += "],\"skipped\":[";
    append_diagnostics(out, result.skipped);
    out += "]}";
    return out;
}

} // namespace tessera::compiler::format
