// SPDX-License-Identifier: MIT
//
// Codegen of inline spans (issue #229, ir-v1.md section 3.6): the parent Text payload carries the
// concatenated text plus one PaintSpan (child node id, byte range) per span, and the span children
// are emitted as ordinary scene nodes so the style cascade and the accessibility tree see their
// class/style/role/events.

#include "codegen_sample_module.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;
using tessera::test::codegen_sample::make_sample_module;
using tessera::test::codegen_sample::span;

int failures = 0;

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string generate(const ir::IrModule& module) {
    const auto result =
        codegen::generate(module, {}, std::nullopt, codegen::EmitMode::GeneratedBase);
    if (!result) {
        std::cerr << "FAIL: the spanned sample generates: " << result.error().front() << "\n";
        ++failures;
        return {};
    }
    std::string out;
    for (const codegen::GeneratedFile& file : *result) {
        out += file.text;
    }
    return out;
}

// Append a Text node with two literal spans (a plain run and a bold link) to the sample root.
ir::IrModule make_span_module() {
    ir::IrModule module = make_sample_module();
    ir::Node& root = module.components[0].root;
    ir::Node text;
    text.kind = ir::NodeKind::Text;
    text.id = "node::90";
    text.ui_id = "90";
    text.source = span(40, 8, 4, 3);
    ir::Node plain;
    plain.kind = ir::NodeKind::Text;
    plain.id = "node::91";
    plain.ui_id = "91";
    plain.source = span(42, 5, 4, 5);
    plain.text = ir::make_literal(std::string("Hello "));
    ir::Node link;
    link.kind = ir::NodeKind::Text;
    link.id = "node::92";
    link.ui_id = "92";
    link.source = span(48, 5, 4, 11);
    link.text = ir::make_literal(std::string("Terms"));
    link.props.emplace("role", ir::make_literal(std::string("link")));
    link.style.emplace("font-weight", ir::make_literal(std::string("700")));
    text.children.push_back(std::move(plain));
    text.children.push_back(std::move(link));
    root.children.push_back(std::move(text));
    return module;
}

void test_spanned_text_codegen() {
    const ir::IrModule module = make_span_module();
    const std::vector<ir::IrDiagnostic> diagnostics = ir::validate(module);
    check(diagnostics.empty(), "the spanned module is valid");
    const std::string text = generate(module);
    if (text.empty()) {
        return;
    }
    check(text.find("paint_") != std::string::npos, "the parent payload is emitted");
    // The parent payload holds the concatenation, written before the children exist.
    check(text.find("\"Hello Terms\"") != std::string::npos,
          "the parent paint text is the concatenation of the span texts");
    // One PaintSpan per child, pushed while the child nodes are created and set afterwards.
    const std::size_t first_span = text.find(".spans.push_back(tessera::ui::PaintSpan{");
    check(first_span != std::string::npos, "the span table is emitted");
    check(first_span != std::string::npos &&
              text.find(", 0u, 6u});", first_span) != std::string::npos,
          "the range of the first span is emitted");
    check(first_span != std::string::npos &&
              text.find(", 6u, 11u});", first_span) != std::string::npos,
          "the range of the second span is emitted");
    check(first_span != std::string::npos && text.find(".spans.push_back(tessera::ui::PaintSpan{",
                                                       first_span + 1) != std::string::npos,
          "one entry per span");
    check(text.find("->set_role(\"link\");") != std::string::npos,
          "the span role is emitted on its own child node");
    check(text.find("->set_inline_style(\"font-weight\", \"700\");") != std::string::npos,
          "the span style is emitted on its own child node");
    // The paint payload is set after the children exist, so the ids are readable.
    const std::size_t paint = text.find(".spans.push_back");
    const std::size_t set = paint == std::string::npos
                                ? std::string::npos
                                : text.find("->set_paint_data(std::move(paint_", paint);
    check(paint != std::string::npos && set != std::string::npos && set > paint,
          "the payload is set after the child ids are read");
}

} // namespace

int main() {
    test_spanned_text_codegen();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
