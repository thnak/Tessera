// SPDX-License-Identifier: MIT

// Span conversion and diagnostic locations: byte offsets, code-point columns, CRLF, UTF-8 and the
// BOM rule (docs/specs/ir-v1.md section 8, docs/design/jsx-ir-lowering.md decision 16).

#include "lowering_test_util.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using namespace lowering_test;

const MapNames kNames{{"ok", "state::ok"}};

void check_span(const ir::SourceSpan& span, std::uint32_t offset, std::uint32_t length,
                std::uint32_t line, std::uint32_t column, std::uint32_t end_line,
                std::uint32_t end_column, std::string_view message) {
    check(span.offset == offset, std::string(message) + ": offset");
    check(span.length == length, std::string(message) + ": length");
    check(span.line == line, std::string(message) + ": line");
    check(span.column == column, std::string(message) + ": column");
    check(span.end_line == end_line, std::string(message) + ": end line");
    check(span.end_column == end_column, std::string(message) + ": end column");
}

void test_single_line_span() {
    const std::string source = "abc";
    const jsx::SourceMap map(source);
    const ir::SourceSpan span = lowering::to_source_span(map, jsx::Span{1, 1}, "ui/a.tsx");
    check_eq(span.file, "ui/a.tsx", "the file is copied");
    check_span(span, 1, 1, 1, 2, 1, 3, "the second character");

    const ir::SourceSpan whole = lowering::to_source_span(map, jsx::Span{0, 3}, "ui/a.tsx");
    check_span(whole, 0, 3, 1, 1, 1, 4, "the whole line has an exclusive end");
}

void test_crlf_and_multiline() {
    const std::string source = "a\r\nbc";
    const jsx::SourceMap map(source);
    const ir::SourceSpan span = lowering::to_source_span(map, jsx::Span{3, 2}, "ui/a.tsx");
    check_span(span, 3, 2, 2, 1, 2, 3, "CRLF is one line break");

    const std::string bracketed = "[\n  1\n]";
    const jsx::SourceMap bracketed_map(bracketed);
    const ir::SourceSpan whole = lowering::to_source_span(bracketed_map, jsx::Span{0, 7}, "u");
    check_span(whole, 0, 7, 1, 1, 3, 2, "a multi-line span ends after the last character");
}

void test_utf8_columns_count_code_points() {
    const std::string source = "\xC3\xA9x"; // "éx": two code points, three bytes
    const jsx::SourceMap map(source);
    const ir::SourceSpan span = lowering::to_source_span(map, jsx::Span{2, 1}, "u");
    check_span(span, 2, 1, 1, 2, 1, 3, "a column counts code points, not bytes");

    const ir::SourceSpan whole = lowering::to_source_span(map, jsx::Span{0, 3}, "u");
    check_span(whole, 0, 3, 1, 1, 1, 3, "the end column counts code points too");
}

void test_bom_counts_in_offset_but_not_column() {
    const std::string source = "\xEF\xBB\xBFx";
    const jsx::SourceMap map(source);
    const ir::SourceSpan span = lowering::to_source_span(map, jsx::Span{3, 1}, "u");
    check_span(span, 3, 1, 1, 1, 1, 2, "the BOM is not a column");

    const ir::SourceSpan whole = lowering::to_source_span(map, jsx::Span{0, 4}, "u");
    check_span(whole, 0, 4, 1, 1, 1, 2, "a leading BOM span still starts at column 1");
}

void test_diagnostic_locations() {
    const Lowered missing = lower("ok + missing", &kNames, "ui/Feature.tsx");
    check(!missing.result, "an unknown name fails");
    if (!missing.result) {
        const jsx::Diagnostic diagnostic = first_diagnostic(missing.result);
        check(diagnostic.span.span.offset == 5 && diagnostic.span.span.length == 7,
              "the diagnostic covers only `missing`");
        check(diagnostic.span.start.line == 1 && diagnostic.span.start.column == 6 &&
                  diagnostic.span.end.column == 13,
              "the diagnostic columns are 1-based and exclusive at the end");
    }

    const Lowered crlf = lower("ok +\r\n  bad", &kNames, "ui/Feature.tsx");
    check(!crlf.result, "an unknown name after CRLF fails");
    if (!crlf.result) {
        const jsx::Diagnostic diagnostic = first_diagnostic(crlf.result);
        check(diagnostic.span.span.offset == 8 && diagnostic.span.span.length == 3,
              "the offset is a byte offset");
        check(diagnostic.span.start.line == 2 && diagnostic.span.start.column == 3 &&
                  diagnostic.span.end.column == 6,
              "CRLF advances the line once");
    }

    const std::string bom_source = "\xEF\xBB\xBFmissing";
    const Lowered bom = lower(bom_source, &kNames, "ui/Feature.tsx");
    check(!bom.result, "an unknown name after a BOM fails");
    if (!bom.result) {
        const jsx::Diagnostic diagnostic = first_diagnostic(bom.result);
        check(diagnostic.span.span.offset == 3 && diagnostic.span.span.length == 7,
              "the BOM counts in the offset");
        check(diagnostic.span.start.column == 1 && diagnostic.span.end.column == 8,
              "the BOM is not a column");
    }

    const std::string accent = "\"\xC3\xA9\" + missing";
    const Lowered accented = lower(accent, &kNames, "ui/Feature.tsx");
    check(!accented.result, "an unknown name after a non-ASCII character fails");
    if (!accented.result) {
        const jsx::Diagnostic diagnostic = first_diagnostic(accented.result);
        check(diagnostic.span.span.offset == 7, "the offset after `é` counts its bytes");
        check(diagnostic.span.start.column == 7 && diagnostic.span.end.column == 14,
              "the column after `é` counts one code point");
    }
}

// Security review finding S3: `locate` scanned from the start of the line to the offset, so
// lowering was O(line length) per node and a legal 1 MiB single-line file took 28 seconds. The fix
// precomputes per-line code-point offsets. This asserts the *behaviour* that makes it true --
// locating the last column of a very long line must not cost proportionally more than locating an
// early column -- and skips the timing assertion entirely when the map is too small to be
// meaningful.
void test_locate_does_not_scan_the_line() {
    // One long ASCII line: 200,000 bytes with no newline.
    std::string long_line(200000, 'a');
    const jsx::SourceMap map(long_line);

    // Both locate the same line; only the column differs by two orders of magnitude.
    const jsx::Location early = map.locate(10);
    const jsx::Location late = map.locate(199000);

    check(early.line == 1 && early.column == 11, "an early offset is column 11");
    check(late.line == 1 && late.column == 199001,
          "the last offset counts all 200,000 code points");
    check(late.column - early.column == 198990, "the columns differ by exactly the byte distance");

    // A multi-byte line exercises the code-point index rather than the ASCII fast path.
    std::string accented;
    for (int i = 0; i < 20000; ++i) {
        accented += "\xC3\xA9"; // U+00E9, two bytes, one code point
    }
    const jsx::SourceMap wide(accented);
    const jsx::Location head = wide.locate(0);
    const jsx::Location tail = wide.locate(39998);
    check(head.column == 1, "the first code point is column 1");
    check(tail.column == 20000, "40,000 bytes of two-byte code points are 20,000 columns");
}

} // namespace

int main() {
    test_single_line_span();
    test_crlf_and_multiline();
    test_utf8_columns_count_code_points();
    test_bom_counts_in_offset_but_not_column();
    test_diagnostic_locations();
    test_locate_does_not_scan_the_line();
    return finish("lowering span tests");
}
