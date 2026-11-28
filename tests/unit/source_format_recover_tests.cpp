// SPDX-License-Identifier: MIT

// Range formatting for .tsx files with recoverable errors (ADR-061 slice 4,
// docs/specs/source-format-v1.md section 14): the items around a broken one are formatted, the
// broken region stays byte for byte, no comment is lost, and anything the parser cannot confine to
// items is still refused.

#include "source_format_test_support.hpp"
#include "tessera/compiler/format/edits.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <string>
#include <string_view>

namespace source_format_test {

namespace format = tessera::compiler::format;

namespace {

format::FormatOptions recovering() {
    format::FormatOptions options;
    options.recover = true;
    return options;
}

constexpr std::string_view kBroken =
    "export function Bad() {\n  return <div  a='x'>oops </span>;\n}\n";

std::string sample() {
    return std::string("import {useState} from 'tessera';\n\n"
                       "// keeps its place\n"
                       "export function Good( ){return <View   a='x'/>;}\n\n") +
           std::string(kBroken) +
           "\n// before Last\n"
           "export function Last(){const  n=1; return <Text>{n}</Text>;} // tail\n";
}

void test_one_broken_item() {
    const std::string input = sample();
    check(!format::format_source(format::FileKind::Tsx, input).ok(),
          "without recover a file with a broken item is refused");

    const auto result = format::format_source(format::FileKind::Tsx, input, recovering());
    check(result.ok(), "with recover the file is formatted");
    check(result.skipped.size() == 1 && result.skipped[0].code == "TESSERA_FMT_SKIPPED",
          "one region is reported as skipped");
    check(result.skipped.size() == 1 && result.skipped[0].line == 6,
          "the skipped region is located at its first line");
    check(result.text.find(std::string(kBroken).substr(0, kBroken.size() - 1)) != std::string::npos,
          "the broken region is byte-identical: " + result.text);
    check(result.text.find("import { useState } from \"tessera\";") != std::string::npos,
          "the item before is formatted");
    check(result.text.find("export function Last() {") != std::string::npos &&
              result.text.find("const n = 1;") != std::string::npos,
          "the item after is formatted");
    for (const char* comment : {"// keeps its place", "// before Last", "// tail"}) {
        check(result.text.find(comment) != std::string::npos,
              std::string("comment kept: ") + comment);
    }
    check(result.text.find("// keeps its place\nexport function Good()") != std::string::npos,
          "a comment keeps leading the item it led: " + result.text);
    check(result.text.find("}\n\nexport function Bad") != std::string::npos,
          "the blank line before the broken region is kept");
}

void test_idempotent() {
    const auto once = format::format_source(format::FileKind::Tsx, sample(), recovering());
    const auto twice = format::format_source(format::FileKind::Tsx, once.text, recovering());
    check(twice.ok() && twice.text == once.text && !twice.changed,
          "recovering format is idempotent");
    check(twice.skipped.size() == 1, "the second pass still reports the region");
}

void test_crlf_and_bom() {
    std::string crlf = "\xEF\xBB\xBF";
    for (const char c : sample()) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    const auto result = format::format_source(format::FileKind::Tsx, crlf, recovering());
    check(result.ok() && result.text.starts_with("\xEF\xBB\xBF"), "the BOM is kept");
    check(result.text.find("<div  a='x'>oops </span>;\r\n}") != std::string::npos,
          "the broken region keeps its CRLF");
    bool bare_lf = false;
    for (std::size_t i = 0; i < result.text.size(); ++i) {
        bare_lf = bare_lf || (result.text[i] == '\n' && (i == 0 || result.text[i - 1] != '\r'));
    }
    check(!bare_lf, "line endings stay CRLF");
}

void test_unrecoverable() {
    const auto all =
        format::format_source(format::FileKind::Tsx, std::string(kBroken), recovering());
    check(!all.ok() && all.diagnostics[0].code == "TESSERA_FMT_SYNTAX" && all.text.empty(),
          "a file with nothing but broken items has nothing to recover and refuses");

    const auto empty_module = format::format_source(format::FileKind::Tsx, "", recovering());
    check(empty_module.ok() && !empty_module.changed, "an empty file is still fine");

    // An error outside any item (a stray token after the last item is part of the last item here,
    // so use a lexer limit instead): a file too large to read has no tree at all.
    const std::string huge(tessera::compiler::jsx::Limits{}.max_file_bytes + 1, ' ');
    const auto too_big = format::format_source(format::FileKind::Tsx, huge, recovering());
    check(!too_big.ok() && too_big.diagnostics[0].code == "TESSERA_FMT_SYNTAX",
          "an unreadable file still refuses");
}

void test_edits_and_json() {
    const std::string input = sample();
    const auto edits = format::format_edits(format::FileKind::Tsx, input, recovering());
    check(edits.ok() && !edits.edits.empty() && edits.skipped.size() == 1,
          "the edit list carries edits and the skipped region");
    const auto bad_at = input.find("export function Bad");
    for (const auto& e : edits.edits) {
        check(e.offset + e.length <= bad_at || e.offset >= bad_at + kBroken.size() - 1,
              "no edit touches the broken region");
    }
    const std::string json = format::format_edits_json(format::FileKind::Tsx, input, recovering());
    check(json.find("\"diagnostics\":[],\"skipped\":[{\"code\":\"TESSERA_FMT_SKIPPED\"") !=
              std::string::npos,
          "wire form lists skipped: " + json);
    check(format::format_edits_json(format::FileKind::Tsx, "const a='x';\n")
              .ends_with("\"diagnostics\":[],\"skipped\":[]}"),
          "wire form of a clean file has an empty skipped list");
}

void test_utf16_offsets() {
    const std::string input =
        std::string("// \xF0\x9F\x98\x80\nconst  a=1;\n\n") + std::string(kBroken);
    const auto edits = format::format_edits(format::FileKind::Tsx, input, recovering());
    // "// " is 3 units, the astral character 2 and the newline 1, then `const` and the first space:
    // the redundant second space is at unit 12 (byte 14).
    check(edits.ok() && !edits.edits.empty() && edits.edits[0].offset == 12,
          "edit offsets count UTF-16 units: " +
              std::to_string(edits.edits.empty() ? 0 : edits.edits[0].offset));
}

} // namespace

void run_recover_tests() {
    test_one_broken_item();
    test_idempotent();
    test_crlf_and_bom();
    test_unrecoverable();
    test_edits_and_json();
    test_utf16_offsets();
}

} // namespace source_format_test
