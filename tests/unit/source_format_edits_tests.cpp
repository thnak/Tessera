// SPDX-License-Identifier: MIT

// The edit-list output of the formatter (ADR-061 slice 2, docs/specs/source-format-v1.md section
// 11): applying the list equals the whole-file result, the list is minimal, and offsets are UTF-16
// units.

#include "source_format_test_support.hpp"
#include "tessera/compiler/format/edits.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace source_format_test {

namespace format = tessera::compiler::format;

namespace {

std::u16string to_utf16(std::string_view text) {
    std::u16string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = c;
        std::size_t len = 1;
        if (c >= 0xF0U) {
            cp = c & 0x07U;
            len = 4;
        } else if (c >= 0xE0U) {
            cp = c & 0x0FU;
            len = 3;
        } else if (c >= 0xC0U) {
            cp = c & 0x1FU;
            len = 2;
        }
        for (std::size_t k = 1; k < len && i + k < text.size(); ++k) {
            cp = (cp << 6U) | (static_cast<unsigned char>(text[i + k]) & 0x3FU);
        }
        i += len;
        if (cp >= 0x10000U) {
            cp -= 0x10000U;
            out.push_back(static_cast<char16_t>(0xD800U + (cp >> 10U)));
            out.push_back(static_cast<char16_t>(0xDC00U + (cp & 0x3FFU)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string to_utf8(const std::u16string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t cp = text[i];
        if (cp >= 0xD800U && cp < 0xDC00U && i + 1 < text.size()) {
            cp = 0x10000U + ((cp - 0xD800U) << 10U) + (text[++i] - 0xDC00U);
        }
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }
    return out;
}

// Applies an edit list the way an editor does: every edit is in the coordinates of the original
// text, so they are applied from the last to the first. Also checks the list is ascending and
// non-overlapping, which is what makes that order irrelevant.
std::string apply(std::string_view text, const std::vector<format::TextEdit>& edits,
                  const std::string& what) {
    std::u16string u = to_utf16(text);
    std::uint32_t previous_end = 0;
    for (const format::TextEdit& e : edits) {
        check(e.offset >= previous_end, what + ": edits ascend and do not overlap");
        check(e.offset + e.length <= u.size(), what + ": edit inside the text");
        previous_end = e.offset + e.length;
    }
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        if (it->offset + it->length > u.size()) {
            return {};
        }
        u.replace(it->offset, it->length, to_utf16(it->new_text));
    }
    return to_utf8(u);
}

void test_apply_equals_whole_file() {
    const std::vector<std::string> inputs = {
        "const a = 1;\n",
        "const   a=1\n\n\n\nconst b  =  'x'\n",
        "export function App(){return <div   className='a'>hi</div>}\n",
        "// caf\xC3\xA9 \xF0\x9F\x98\x80\nconst s = 'na\xC3\xAFve \xF0\x9F\x98\x80'   ;\nconst "
        "t=2\n",
        "const a=1;\r\nconst  b=2;\r\n",
        "\xEF\xBB\xBF"
        "const  a=1\n",
        "",
    };
    for (const std::string& input : inputs) {
        const format::FormatResult whole = format::format_source(format::FileKind::Tsx, input);
        const format::FormatEditsResult edits = format::format_edits(format::FileKind::Tsx, input);
        check(whole.ok() == edits.ok(), "edit list refuses exactly when the whole file does");
        if (!whole.ok()) {
            continue;
        }
        check(apply(input, edits.edits, "apply") == whole.text,
              "applying the edits equals the whole-file text");
        check(edits.edits.empty() == !whole.changed, "no edits exactly when nothing changed");
    }
}

void test_minimal() {
    const std::string canonical = "const a = 1;\n";
    check(format::format_edits(format::FileKind::Tsx, canonical).edits.empty(),
          "a canonical file gets an empty edit list");

    // Only the two quote characters change: two one-unit edits, nothing in between touched.
    const std::string quotes = "const a = 'x';\nconst b = 2;\n";
    const auto q = format::format_edits(format::FileKind::Tsx, quotes).edits;
    check(q.size() == 2, "a quote change is two edits");
    if (q.size() == 2) {
        check(q[0].offset == 10 && q[0].length == 1 && q[0].new_text == "\"", "opening quote edit");
        check(q[1].offset == 12 && q[1].length == 1 && q[1].new_text == "\"", "closing quote edit");
    }

    // Spacing: only the whitespace and the missing spaces around the operator change.
    const auto s = format::format_edits(format::FileKind::Tsx, "const a=1;\n").edits;
    check(s.size() == 2, "spacing around = is two insertions");
    for (const format::TextEdit& e : s) {
        check(e.length == 0 && e.new_text == " ", "an insertion of one space");
    }

    // A change far from the start leaves everything before it alone, and the offset counts the
    // surrogate pair of the emoji as two units.
    const std::string late = "// \xF0\x9F\x98\x80\nconst a = 1;\nconst   b = 2;\n";
    const auto l = format::format_edits(format::FileKind::Tsx, late).edits;
    check(l.size() == 1, "one hunk of extra spaces");
    if (l.size() == 1) {
        check(l[0].offset == 3 + 2 + 1 + 13 + 5 + 1 && l[0].length == 2 && l[0].new_text.empty(),
              "offset is in UTF-16 units: the emoji counts as two");
    }
}

void test_diff_properties() {
    std::uint32_t seed = 12345;
    const auto next = [&seed](std::uint32_t bound) {
        seed = seed * 1664525U + 1013904223U;
        return (seed >> 8U) % bound;
    };
    const std::vector<std::string> pieces = {
        "a", "bc", " ", "  ", "\n", "+", "(", ")", "\xC3\xA9", "\xF0\x9F\x98\x80", "foo", "'"};
    for (int round = 0; round < 200; ++round) {
        std::string before;
        std::string after;
        const std::uint32_t len = next(30);
        for (std::uint32_t i = 0; i < len; ++i) {
            const std::string& piece = pieces[next(static_cast<std::uint32_t>(pieces.size()))];
            before += piece;
            after += next(4) == 0 ? pieces[next(static_cast<std::uint32_t>(pieces.size()))] : piece;
        }
        const auto edits = format::diff_text_edits(before, after);
        check(apply(before, edits, "random pair") == after, "diff_text_edits round-trips");
        check(format::diff_text_edits(before, before).empty(), "equal texts have no edits");
    }
    // Large unrelated texts fall back to one edit instead of running away.
    const std::string big_a(5000, 'a');
    std::string big_b;
    for (int i = 0; i < 5000; ++i) {
        big_b += i % 2 == 0 ? "b " : "c(";
    }
    check(apply(big_a, format::diff_text_edits(big_a, big_b), "fallback") == big_b,
          "fallback edit round-trips");
}

void test_refusal_and_json() {
    const std::string broken = "const = ;\n";
    const format::FormatEditsResult refused = format::format_edits(format::FileKind::Tsx, broken);
    check(!refused.ok() && refused.edits.empty(), "a refused file has diagnostics and no edits");
    const std::string json = format::format_edits_json(format::FileKind::Tsx, broken);
    check(json.starts_with("{\"edits\":[],\"diagnostics\":[{\"code\":\"TESSERA_FMT_SYNTAX\""),
          "refusal json: " + json);

    const std::string ok = format::format_edits_json(format::FileKind::Tsx, "const a='x\\\"';\n");
    check(ok.find("\"diagnostics\":[]") != std::string::npos,
          "success json has no diagnostics: " + ok);
}

} // namespace

void run_edits_tests() {
    test_apply_equals_whole_file();
    test_minimal();
    test_diff_properties();
    test_refusal_and_json();
}

} // namespace source_format_test
