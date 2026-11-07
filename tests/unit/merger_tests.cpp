// SPDX-License-Identifier: MIT

#include <tessera/compiler/merger.hpp>

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_extract_regions() {
    const std::string text = "// [DEVELOPER_LOGIC_START: bind::on_click]\n"
                             "void onClick() {}\n"
                             "// [DEVELOPER_LOGIC_END: bind::on_click]\n";

    const auto regions = tessera::compiler::extract_regions(text);
    check(regions.size() == 1, "extract: one region");
    check(regions[0].id == "bind::on_click", "extract: id");
    check(regions[0].body == "void onClick() {}\n", "extract: body");
}

void test_merge_preserves_developer_logic() {
    const std::string generated = "// generated\n"
                                  "struct Widget {\n"
                                  "    // [DEVELOPER_LOGIC_START: bind::on_click]\n"
                                  "    // [DEVELOPER_LOGIC_END: bind::on_click]\n"
                                  "};\n";

    const std::string existing = "// old\n"
                                 "struct Widget {\n"
                                 "    // [DEVELOPER_LOGIC_START: bind::on_click]\n"
                                 "    void onClick() { doThing(); }\n"
                                 "    // [DEVELOPER_LOGIC_END: bind::on_click]\n"
                                 "};\n";

    const auto merged = tessera::compiler::merge(generated, existing);
    check(merged.text.find("void onClick() { doThing(); }") != std::string::npos,
          "merge: developer body preserved");
    check(merged.text.find("// generated") != std::string::npos,
          "merge: generated scaffolding kept");
    check(merged.warnings.empty(), "merge: no warnings");
}

void test_merge_is_idempotent() {
    const std::string generated = "struct Widget {\n"
                                  "    // [DEVELOPER_LOGIC_START: bind::a]\n"
                                  "    // [DEVELOPER_LOGIC_END: bind::a]\n"
                                  "    // [DEVELOPER_LOGIC_START: bind::b]\n"
                                  "    // [DEVELOPER_LOGIC_END: bind::b]\n"
                                  "};\n";

    const std::string existing = "struct Widget {\n"
                                 "    // [DEVELOPER_LOGIC_START: bind::a]\n"
                                 "    int a = 1;\n"
                                 "    // [DEVELOPER_LOGIC_END: bind::a]\n"
                                 "    // [DEVELOPER_LOGIC_START: bind::b]\n"
                                 "    int b = 2;\n"
                                 "    // [DEVELOPER_LOGIC_END: bind::b]\n"
                                 "};\n";

    const auto first = tessera::compiler::merge(generated, existing);
    const auto second = tessera::compiler::merge(first.text, existing);
    const auto third = tessera::compiler::merge(second.text, existing);

    check(first.text == second.text, "idempotent: merge(g,e) == merge(merge(g,e),e)");
    check(second.text == third.text, "idempotent: repeated merges stay stable");
    check(first.text.find("int a = 1;") != std::string::npos, "idempotent: region a preserved");
    check(first.text.find("int b = 2;") != std::string::npos, "idempotent: region b preserved");
}

void test_missing_region_warns() {
    const std::string generated = "// no protected regions here\n";
    const std::string existing = "// [DEVELOPER_LOGIC_START: bind::gone]\n"
                                 "int gone = 1;\n"
                                 "// [DEVELOPER_LOGIC_END: bind::gone]\n";

    const auto merged = tessera::compiler::merge(generated, existing);
    check(merged.text == generated, "missing region: generated output unchanged");
    check(merged.warnings.size() == 1, "missing region: one warning");
}

void test_crlf_body_is_preserved() {
    const std::string text = "// [DEVELOPER_LOGIC_START: bind::x]\r\n"
                             "abc\r\n"
                             "// [DEVELOPER_LOGIC_END: bind::x]\r\n";

    const auto regions = tessera::compiler::extract_regions(text);
    check(regions.size() == 1, "crlf: one region");
    check(regions[0].body == "abc\r\n", "crlf: body bytes preserved");
}

} // namespace

int main() {
    test_extract_regions();
    test_merge_preserves_developer_logic();
    test_merge_is_idempotent();
    test_missing_region_warns();
    test_crlf_body_is_preserved();

    if (failures == 0) {
        std::cout << "merger tests passed\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed\n";
    return 1;
}
