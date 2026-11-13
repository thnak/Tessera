// SPDX-License-Identifier: MIT

// ADR-040 generated base (issue #157, slice 1): shape, golden and determinism. The compile side (a
// developer class deriving from the base, and the compile-fail checks) lives in
// codegen_base_runtime_tests.cpp and the codegen_base_* targets.
//
// Deliberate golden update path: `codegen_base_tests --write` rewrites
// tests/golden/codegen/Sample.g.hpp.

#include "codegen_sample_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_CODEGEN_GOLDEN_DIR
#error "TESSERA_CODEGEN_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace codegen = tessera::compiler::codegen;
using tessera::test::codegen_sample::make_sample_module;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

codegen::CodegenResult generate_base() {
    return codegen::generate(make_sample_module(), {}, std::nullopt,
                             codegen::EmitMode::GeneratedBase);
}

void test_shape() {
    const auto result = generate_base();
    check(result.has_value(), "the sample generates in base mode");
    if (!result) {
        std::cerr << "  error: " << result.error().front() << "\n";
        return;
    }
    check(result->size() == 1, "base mode emits one header-only file per component");
    if (result->empty()) {
        return;
    }
    const std::string& text = result->front().text;
    check(result->front().path == "Sample.g.hpp", "deterministic path");
    check(contains(text, "struct SampleBase {") && contains(text, "template <class Self>"),
          "a CRTP template");
    check(!contains(text, "[DEVELOPER_LOGIC_START"), "no protected regions in the base");
    check(contains(text, "void sync() { return static_cast<Self&>(*this).sync(); }"),
          "a dispatch thunk per effect binding");
    check(contains(text, "void on_click(const tessera::ui::ClickEvent& event) { return "
                         "static_cast<Self&>(*this).on_click(event); }"),
          "a dispatch thunk per event binding, typed by its payload");
    check(contains(text, "static_assert(") && contains(text, "TESSERA_CG_MISSING_BINDING") &&
              contains(text, "bind::on_click") && contains(text, "ui/Counter.tsx:"),
          "build() checks each binding and names the id and the TSX location");
    check(contains(text, "concept SampleContract"), "a contract concept for IDE tooling");
    check(contains(text, "[[nodiscard]] const int& count() const") &&
              contains(text, "tessera::ui::Node* ref_box() const") &&
              contains(text, "const std::string& title() const"),
          "accessors named after the TSX");
    check(contains(text, "inline void SampleBase<Self>::build("),
          "definitions are inline templates in the header");
}

void test_determinism() {
    const auto first = generate_base();
    const auto second = generate_base();
    check(first && second && first->front().text == second->front().text,
          "base output is byte-identical across runs");
}

void test_golden(bool update) {
    const auto result = generate_base();
    if (!result || result->empty()) {
        check(false, "base golden needs a generated file");
        return;
    }
    const std::filesystem::path path =
        std::filesystem::path(TESSERA_CODEGEN_GOLDEN_DIR) / "Sample.g.hpp";
    if (update) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << result->front().text;
        return;
    }
    check(read_file(path) == result->front().text,
          "Sample.g.hpp matches its golden; run `codegen_base_tests --write`");
}

// scaffold() and required_bindings() (ADR-040 decision item 6): one developer class per component,
// one member per binding, sorted and deterministic.
void test_scaffold() {
    const auto module = make_sample_module();
    const auto stubs = codegen::scaffold(module);
    check(stubs.has_value(), "scaffold succeeds on the sample module");
    if (!stubs) {
        return;
    }
    check(stubs->size() == 2 && stubs->front().path == "Sample.hpp" &&
              stubs->back().path == "Sample.cpp",
          "scaffold returns Sample.hpp then Sample.cpp");
    const std::string& header = stubs->front().text;
    check(header.find("class Sample final : public gen::SampleBase<Sample>") != std::string::npos,
          "the stub derives from the CRTP base");
    check(header.find("friend struct gen::SampleBase<Sample>;") != std::string::npos,
          "the stub befriends its base so the members can stay private");
    check(header.find("#include \"Sample.g.hpp\"") != std::string::npos,
          "the stub includes the generated base");
    std::string error;
    const auto required = codegen::required_bindings(module, error);
    check(error.empty() && !required.empty(), "required_bindings lists the sample bindings");
    for (const codegen::RequiredBinding& binding : required) {
        check(header.find(binding.signature) != std::string::npos,
              "every required binding is declared in the stub");
        check(stubs->back().text.find("Sample::" + binding.member + "(") != std::string::npos,
              "every required binding is defined in the stub source");
    }
    const auto again = codegen::scaffold(module);
    check(again && again->front().text == header && again->back().text == stubs->back().text,
          "scaffold is deterministic");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--write") {
        test_golden(true);
        return failures == 0 ? 0 : 1;
    }
    test_shape();
    test_determinism();
    test_scaffold();
    test_golden(false);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen base tests passed\n";
    return 0;
}
