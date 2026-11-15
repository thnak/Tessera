// SPDX-License-Identifier: MIT
//
// `#line` mapping to TSX (issue #78): the directives the generated base carries when
// `CodegenOptions::line_directives` is on, the generated-file numbering the map restores after a
// mapped region, and the default-off guarantee that keeps the committed goldens byte-identical.
//
// The real before/after compiler error is proven by `codegen_line_map_compile_tests`, which drives
// the compiler over the mapped header and asserts the diagnostic location; this file owns the text
// contract the map depends on.

#include "codegen_sample_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;
using tessera::test::codegen_sample::make_sample_module;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string mapped() {
    const auto result = codegen::generate(make_sample_module(), {}, std::nullopt,
                                          codegen::CodegenOptions{.line_directives = true});
    if (!result || result->empty()) {
        check(false, "the sample generates with line directives");
        return {};
    }
    return result->front().text;
}

std::string unmapped() {
    const auto result = codegen::generate(make_sample_module());
    if (!result || result->empty()) {
        check(false, "the sample generates without line directives");
        return {};
    }
    return result->front().text;
}

// Counts the lines of `text` up to and including `limit` lines.
std::size_t count_lines(const std::string& text) {
    std::size_t lines = 0;
    for (const char c : text) {
        lines += c == '\n' ? 1 : 0;
    }
    return lines;
}

void test_default_is_byte_identical() {
    // The whole reason the flag exists: the committed `.g.hpp` goldens must not move.
    check(unmapped().find("#line ") == std::string::npos,
          "no directive reaches the generated base by default");
    check(unmapped().find("//@tessera-line") == std::string::npos,
          "no marker leaks into the default output");
}

void test_directives() {
    const std::string text = mapped();
    if (text.empty()) {
        return;
    }
    check(text.find("//@tessera-line") == std::string::npos, "no marker survives resolution");
    // The sample's three bindings all sit at ui/Counter.tsx:4:3, so the mapping is that line.
    check(text.find("#line 4 \"ui/Counter.tsx\"") != std::string::npos,
          "a binding's static_assert maps to its TSX line");
    // Each mapped region is closed by a directive naming the generated file again.
    check(text.find("#line ") != std::string::npos &&
              text.find("\"Sample.g.hpp\"") != std::string::npos,
          "the generated numbering is restored after a mapped region");
    // The message still names the location, so the map and the text cannot disagree.
    check(text.find("(ui/Counter.tsx:4:3)") != std::string::npos,
          "the contract message keeps its TSX location");
    check(count_lines(text) > count_lines(unmapped()), "directives add physical lines to the file");
}

// The invariant that makes the second error as accurate as the first: outside a mapped region the
// line number a `#line` restore names IS the physical line it sits on, so generated code keeps its
// own coordinates instead of drifting by the directives emitted above it.
void test_restore_is_self_consistent() {
    const std::string text = mapped();
    if (text.empty()) {
        return;
    }
    std::size_t restore_directives = 0;
    std::size_t physical = 1;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line(text.data() + at,
                                    (end == std::string::npos ? text.size() : end) - at);
        const std::string_view marker = "#line ";
        if (line.substr(0, marker.size()) == marker) {
            // `#line N "file"` names the line that FOLLOWS it.
            const std::string number(
                line.substr(marker.size(), line.find(' ', marker.size()) - marker.size()));
            const auto named = static_cast<std::size_t>(std::stoul(number));
            const bool generated = line.find("\"Sample.g.hpp\"") != std::string_view::npos;
            if (generated) {
                ++restore_directives;
                check(named == physical + 1,
                      "a restoring directive names the physical line that follows it");
            }
        }
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
        ++physical;
    }
    check(restore_directives == 3, "one restore per mapped binding region");
}

// Two mapped regions, so a file with more than one diagnostic is exercised (the lead's requirement:
// test two mapped errors, not one).
void test_two_regions() {
    const std::string text = mapped();
    if (text.empty()) {
        return;
    }
    std::vector<std::size_t> tsx_lines;
    for (std::size_t at = text.find("#line 4 \"ui/Counter.tsx\""); at != std::string::npos;
         at = text.find("#line 4 \"ui/Counter.tsx\"", at + 1)) {
        tsx_lines.push_back(at);
    }
    check(tsx_lines.size() == 3, "one TSX directive per binding, three in the sample");
    check(tsx_lines.size() >= 2 && tsx_lines[1] > tsx_lines[0], "the second region is later");
    // The `static_assert` of a mapped region sits directly below its directive.
    const std::size_t first = tsx_lines.empty() ? std::string::npos : tsx_lines.front();
    check(first != std::string::npos && text.find("static_assert(", first) > first &&
              text.find("static_assert(", first) - first < 64,
          "the mapped statement follows its directive");
}

// A module whose bindings carry no location (a hand-built IR) must not invent one.
void test_unmappable_span_emits_nothing() {
    ir::IrModule module = make_sample_module();
    for (ir::Binding& binding : module.bindings) {
        binding.source = ir::SourceSpan{};
    }
    const auto result = codegen::generate(module, {}, std::nullopt,
                                          codegen::CodegenOptions{.line_directives = true});
    check(result.has_value() && !result->empty(), "the module still generates");
    if (!result || result->empty()) {
        return;
    }
    const std::string& text = result->front().text;
    check(text.find("//@tessera-line") == std::string::npos, "no marker survives");
    check(text.find("#line 0") == std::string::npos, "no directive is emitted for a missing span");
    check(text.find("#line ") == std::string::npos,
          "an unmappable binding produces no directive at all");
}

} // namespace

int main() {
    test_default_is_byte_identical();
    test_directives();
    test_restore_is_self_consistent();
    test_two_regions();
    test_unmappable_span_emits_nothing();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen line map tests passed\n";
    return 0;
}
