// SPDX-License-Identifier: MIT

// Component composition and the static `Children` slot in codegen (docs/specs/ir-v1.md section 3.4,
// docs/design/compiler.md section 11): the caller's child nodes become the instance's slot content,
// the callee attaches them at its Slot, every instance gets its own generated struct member, and
// the deferred instance forms are rejected loudly.
//
// The fixture is hand-built IR: `Card` declares `{ children: Children }` and places `{children}`,
// `CardPage` instantiates `Card` with a text, a nested view and an event-bound Button. The golden
// update path is `codegen_composition_tests --write`.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/merger.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_CODEGEN_GOLDEN_DIR
#error "TESSERA_CODEGEN_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace ir = tessera::compiler::ir;
namespace codegen = tessera::compiler::codegen;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(const std::string& actual, const std::string& expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                    std::uint32_t column, std::uint32_t end_line, std::uint32_t end_column) {
    ir::SourceSpan value;
    value.file = "ui/App.tsx";
    value.offset = offset;
    value.length = length;
    value.line = line;
    value.column = column;
    value.end_line = end_line;
    value.end_column = end_column;
    return value;
}

ir::ExprPtr text(std::string value) {
    return ir::make_literal(std::move(value));
}

ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id, ir::SourceSpan source) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = std::move(source);
    return node;
}

ir::IrModule make_composition_module() {
    ir::IrModule module;
    module.name = "demo";

    // Card: declares the slot and places it after its own text.
    ir::Component card;
    card.id = "comp::Card";
    card.component_id = "51";
    card.name = "Card";
    card.source = span(0, 120, 1, 1, 12, 2);
    card.props.push_back({"children", "children", nullptr});
    card.root = node_of(ir::NodeKind::View, "node::1", "101", span(20, 60, 3, 3, 3, 60));
    card.root.props.emplace("className", text("card"));
    ir::Node card_text = node_of(ir::NodeKind::Text, "node::2", "102", span(30, 10, 3, 5, 3, 15));
    card_text.text = text("card");
    card.root.children.push_back(std::move(card_text));
    card.root.children.push_back(
        node_of(ir::NodeKind::Slot, "node::3", "103", span(45, 10, 3, 20, 3, 30)));

    // CardPage: one instance of Card with text, a nested view and an event-bound Button.
    ir::Component page;
    page.id = "comp::CardPage";
    page.component_id = "52";
    page.name = "CardPage";
    page.source = span(200, 140, 14, 1, 30, 2);
    page.root = node_of(ir::NodeKind::View, "node::1", "201", span(220, 90, 16, 3, 16, 90));
    page.root.props.emplace("className", text("page"));
    ir::Node instance =
        node_of(ir::NodeKind::Component, "node::2", "202", span(230, 60, 16, 5, 16, 60));
    instance.component = "comp::Card";
    ir::Node hello = node_of(ir::NodeKind::Text, "node::3", "203", span(240, 12, 16, 12, 16, 24));
    hello.text = text("hello");
    instance.children.push_back(std::move(hello));
    ir::Node wrapper = node_of(ir::NodeKind::View, "node::4", "204", span(250, 20, 16, 30, 16, 50));
    ir::Node nested = node_of(ir::NodeKind::Text, "node::5", "205", span(255, 8, 16, 36, 16, 44));
    nested.text = text("nested");
    wrapper.children.push_back(std::move(nested));
    instance.children.push_back(std::move(wrapper));
    ir::Node tap = node_of(ir::NodeKind::Button, "node::6", "206", span(260, 25, 16, 55, 16, 80));
    tap.props.emplace("label", text("tap"));
    tap.events.emplace("onClick", "bind::on_tap");
    instance.children.push_back(std::move(tap));
    page.root.children.push_back(std::move(instance));
    module.components.push_back(std::move(card));
    module.components.push_back(std::move(page));

    ir::Binding binding;
    binding.id = "bind::on_tap";
    binding.component = "comp::CardPage";
    binding.kind = ir::BindingKind::Event;
    binding.region = "on_tap";
    binding.source = span(262, 15, 16, 60, 16, 75);
    module.bindings.push_back(std::move(binding));
    return module;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text_stream;
    text_stream << in.rdbuf();
    return text_stream.str();
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

std::string file_text(const std::vector<codegen::GeneratedFile>& files, std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return file.text;
        }
    }
    return {};
}

void test_fixture_is_valid() {
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(make_composition_module())) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the composition fixture is a valid IR module");
}

void test_generated_shape() {
    const auto result = codegen::generate(make_composition_module());
    check(result.has_value(), "the composition fixture generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    check(result->size() == 4, "two components, one header and one source each");
    const std::string card_header = file_text(*result, "Card.hpp");
    const std::string card_source = file_text(*result, "Card.cpp");
    const std::string page_header = file_text(*result, "CardPage.hpp");
    const std::string page_source = file_text(*result, "CardPage.cpp");
    check(!card_header.empty() && !page_header.empty(), "both headers are emitted");

    // The callee declares the slot overload and attaches the caller's nodes at the Slot.
    check(card_header.find("void build(tessera::ui::Node& parent, "
                           "std::vector<std::unique_ptr<tessera::ui::Node>> slot);") !=
              std::string::npos,
          "the children-declaring component takes the slot in build()");
    check(card_source.find("void Card::build(tessera::ui::Node& parent)") != std::string::npos &&
              card_source.find("build(parent, {});") != std::string::npos,
          "the one-argument build is the empty-slot wrapper");
    check(card_source.find("for (auto& slot_node : slot)") != std::string::npos,
          "the callee attaches the caller's content");
    check(card_source.find("node_1->add_child(std::move(slot_node));") != std::string::npos,
          "slot content is appended at the Slot position");
    check(card_header.find("std::string prop_children_") == std::string::npos,
          "the children declaration is not a generated prop field");

    // The caller owns the instance and builds its content in the caller's own code.
    check(page_header.find("#include \"Card.hpp\"") != std::string::npos,
          "the caller header includes the referenced component");
    check(page_header.find("Card child_2_;") != std::string::npos,
          "the instance is a member of the caller (it owns its own state)");
    check(page_source.find("auto node_2 = std::make_unique<tessera::ui::Node>(") !=
              std::string::npos,
          "the instance gets a generated host node");
    check(page_source.find("node_2->set_element_kind(\"Component\");") != std::string::npos,
          "the host node records the Component element kind");
    check(page_source.find("std::vector<std::unique_ptr<tessera::ui::Node>> slot_2;") !=
              std::string::npos,
          "the caller collects its slot content");
    check(page_source.find("this->child_2_.build(*node_2, std::move(slot_2));") !=
              std::string::npos,
          "the instance is built with the caller's slot content");
    check(page_source.find("slot_2.push_back(std::move(node_3));") != std::string::npos,
          "the caller's first child is slot content");
    check(page_source.find("// [DEVELOPER_LOGIC_START: bind::on_tap]") != std::string::npos,
          "event bindings of slot content stay in the caller");
    check(page_source.find("child_2_.mount();") != std::string::npos &&
              page_source.find("child_2_.update();") != std::string::npos &&
              page_source.find("child_2_.unmount();") != std::string::npos,
          "the instance lifecycle is driven by the caller");
}

void test_goldens(bool update) {
    const auto result = codegen::generate(make_composition_module());
    check(result.has_value(), "the composition golden sample generates");
    if (!result) {
        return;
    }
    const std::string directory = TESSERA_CODEGEN_GOLDEN_DIR;
    if (update) {
        std::filesystem::create_directories(directory);
    }
    for (const codegen::GeneratedFile& file : *result) {
        const std::string path = directory + "/" + file.path;
        if (update) {
            write_file(path, file.text);
            continue;
        }
        const std::string golden = read_file(path);
        check(!golden.empty(), "golden " + file.path + " is readable");
        if (golden != file.text) {
            check(false,
                  file.path + " differs from its golden; run `codegen_composition_tests --write`");
        }
    }
    if (update) {
        std::cout << "composition goldens written to " << directory << "\n";
    }
}

void test_determinism() {
    const auto first = codegen::generate(make_composition_module());
    const auto second = codegen::generate(make_composition_module());
    check(first.has_value() && second.has_value(), "the composition sample generates twice");
    if (!first || !second) {
        return;
    }
    check_eq(file_text(*first, "CardPage.cpp"), file_text(*second, "CardPage.cpp"),
             "two runs are byte-identical");
}

void test_deferred_forms_are_rejected() {
    {
        ir::IrModule module = make_composition_module();
        module.components[1].root.children[0].condition = ir::make_literal(true);
        check(!codegen::generate(module).has_value(),
              "a conditional component instance is rejected");
    }
    {
        ir::IrModule module = make_composition_module();
        module.components[1].root.children[0].props.emplace("title", text("x"));
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a prop the target does not declare is rejected");
        check(!result.error().empty() &&
                  result.error().front().find("has no prop `title`") != std::string::npos,
              "the unknown component prop is diagnosed by name");
    }
    {
        ir::IrModule module = make_composition_module();
        module.components[1].root.children[0].children[0].condition = ir::make_literal(true);
        check(!codegen::generate(module).has_value(),
              "a conditional direct child of slot content is deferred");
    }
    {
        ir::IrModule module = make_composition_module();
        module.components[0].props.clear();
        check(!codegen::generate(module).has_value(),
              "slot children without the children prop are rejected");
    }
    {
        ir::IrModule module = make_composition_module();
        module.components[1].root.children[0].component = "comp::Gone";
        check(!codegen::generate(module).has_value(), "an unknown component instance is rejected");
    }
    {
        ir::IrModule module = make_composition_module();
        module.components[1].root.children[0].component = "comp::CardPage";
        check(!codegen::generate(module).has_value(), "a component reference cycle is rejected");
    }
}

void test_merger_idempotency() {
    const auto result = codegen::generate(make_composition_module());
    check(result.has_value(), "the composition sample generates for the merger test");
    if (!result) {
        return;
    }
    const std::string generated = file_text(*result, "CardPage.cpp");
    check(!generated.empty(), "the caller source exists");
    constexpr std::string_view kMarker = "// [DEVELOPER_LOGIC_START: bind::on_tap]";
    const std::size_t marker = generated.find(kMarker);
    check(marker != std::string::npos, "the on_tap region marker exists");
    if (marker == std::string::npos) {
        return;
    }
    std::string existing = generated;
    existing.insert(marker + kMarker.size(), "\n    state_dirty_ = true; // developer body");
    const tessera::compiler::MergeResult first = tessera::compiler::merge(generated, existing);
    check(first.text.find("developer body") != std::string::npos,
          "the developer body survives the merge");
    const tessera::compiler::MergeResult second = tessera::compiler::merge(first.text, existing);
    check_eq(second.text, first.text,
             "merge(merge(g, e), e) == merge(g, e) over the composition output");
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    if (update) {
        test_goldens(true);
        return failures == 0 ? 0 : 1;
    }
    test_fixture_is_valid();
    test_generated_shape();
    test_goldens(false);
    test_determinism();
    test_deferred_forms_are_rejected();
    test_merger_idempotency();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen composition tests passed\n";
    return 0;
}
