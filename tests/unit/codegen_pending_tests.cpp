// SPDX-License-Identifier: MIT

// ADR-067 spike #661, emission side: the generated base of a component with a `Suspense` boundary
// and a transition-marked setter. It asserts the shape (the boundary slots, the swap, the mark, the
// fake resource seams), byte-identical determinism, and the `Feed.g.hpp` golden.
//
// Deliberate golden update path: `codegen_pending_tests --write` rewrites
// tests/golden/codegen/pending/Feed.g.hpp; then review the diff and rebuild.

#include "codegen_pending_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

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

using tessera::test::codegen_pending::make_pending_module;

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

codegen::CodegenResult generate_base(const ir::IrModule& module) {
    return codegen::generate(module, {}, std::nullopt);
}

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

std::string concatenated(const std::vector<codegen::GeneratedFile>& files) {
    std::string out;
    for (const codegen::GeneratedFile& file : files) {
        out += file.path + "\n" + file.text;
    }
    return out;
}

void test_fixture_is_valid() {
    const ir::IrModule module = make_pending_module();
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the pending codegen fixture is a valid IR module");
}

void test_generated_shape() {
    const auto result = generate_base(make_pending_module());
    check(result.has_value(), "the pending sample generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    check(result->size() == 1, "one header-only file per component");
    if (result->size() != 1) {
        return;
    }
    check_eq((*result)[0].path, "Feed.g.hpp", "deterministic generated-base path");
    const std::string& header = (*result)[0].text;

    // The engine model is included, not copied: the generated code runs the same state machine the
    // evaluator runs, which is what makes the differential test meaningful.
    check(header.find("#include \"tessera/ui/pending/suspense.hpp\"") != std::string::npos,
          "the boundary model is included");
    check(header.find("#include \"tessera/ui/pending/transition.hpp\"") != std::string::npos,
          "the transition mark is included");
    check(header.find("#include \"tessera/ui/pending/fake_resource.hpp\"") != std::string::npos,
          "the fake resource is included");

    // The boundary owns both subtrees and the live one, and it is a static node: no scene node of
    // its own, no layout box.
    check(header.find("std::unique_ptr<tessera::ui::Node> suspense_2_content_owned_;") !=
              std::string::npos,
          "the content subtree has an ownership slot");
    check(header.find("std::unique_ptr<tessera::ui::Node> suspense_2_fallback_owned_;") !=
              std::string::npos,
          "the fallback subtree has an ownership slot");
    check(header.find("tessera::ui::Node* suspense_2_live_ = nullptr;") != std::string::npos,
          "the boundary tracks the live subtree");
    check(header.find("tessera::ui::pending::SuspenseBoundary suspense_2_;") != std::string::npos,
          "the boundary owns its model");
    check(header.find("void apply_suspense_2();") != std::string::npos,
          "the boundary swap is declared");

    // The transition mark and the fake resource seams.
    check(header.find("tessera::ui::pending::TransitionMark pending_transition_;") !=
              std::string::npos,
          "the transition mark is a member");
    check(header.find("tessera::ui::pending::FakeResource pending_resource_;") != std::string::npos,
          "the fake resource is a member");
    check(header.find("std::uint64_t pending_begin_fetch();") != std::string::npos,
          "the fetch seam is declared");
    check(header.find("bool pending_resolve(std::uint64_t epoch, int value);") != std::string::npos,
          "the resolve seam is declared");
    check(header.find("bool pending_fail(std::uint64_t epoch);") != std::string::npos,
          "the fail seam is declared");
    check(header.find("void pending_sync();") != std::string::npos, "the sync seam is declared");
    check(header.find("[[nodiscard]] bool pending_is_pending() const;") != std::string::npos,
          "isPending is a generated accessor");
    check(header.find("[[nodiscard]] bool pending_fallback_live() const;") != std::string::npos,
          "the boundary phase is a generated accessor");

    // The swap itself: detach the outgoing subtree, keep it, attach the wanted one at the source
    // position. It is idempotent, so an unchanged phase touches no node.
    check(header.find("void FeedBase<Self>::apply_suspense_2()") != std::string::npos,
          "the swap is emitted");
    check(header.find("if (live != nullptr && live_fallback == fallback) {") != std::string::npos,
          "the swap is idempotent");
    check(
        header.find("std::unique_ptr<tessera::ui::Node> previous = parent->remove_child(*live);") !=
            std::string::npos,
        "the outgoing subtree is detached, not destroyed");
    check(header.find("parent->insert_child(at, std::move(wanted));") != std::string::npos,
          "the wanted subtree is attached at the boundary's source position");
    check(header.find("suspense_2_.reset_reads(1);") != std::string::npos,
          "the boundary registers its read count");

    // The marked setter starts the transition before it assigns, so isPending is true from the
    // moment the change is applied.
    check(header.find("pending_transition_.begin(pending_resource_.epoch());") != std::string::npos,
          "the marked setter starts the transition");
    check(header.find("state_query_ = \"next\";") != std::string::npos,
          "the marked setter still assigns its value");
    const std::size_t begin_at =
        header.find("pending_transition_.begin(pending_resource_.epoch());");
    const std::size_t assign_at = header.find("state_query_ = \"next\";");
    check(begin_at != std::string::npos && assign_at != std::string::npos && begin_at < assign_at,
          "the transition starts before the assignment");

    // The fallback and the content are both built, and the boundary attaches one of them.
    check(header.find("// node::5") != std::string::npos, "the fallback subtree is built");
    check(header.find("// node::3") != std::string::npos, "the content subtree is built");
    check(header.find("this->suspense_2_content_owned_ = std::move(node_3);") != std::string::npos,
          "the content subtree lands in its slot");
    check(header.find("this->suspense_2_fallback_owned_ = std::move(node_5);") != std::string::npos,
          "the fallback subtree lands in its slot");
    // The boundary is not painted: it is a static node, so it never becomes a scene child. The
    // generated `node_2` is the boundary's own bookkeeping node, and it is never attached.
    check(header.find("node_2->add_child") == std::string::npos,
          "the boundary attaches no scene child of its own");
    check(header.find("add_child(std::move(node_2))") == std::string::npos,
          "the boundary is never attached to its parent");
}

void test_off_by_default() {
    // A component with neither a Suspense node nor a marked action must generate no pending code at
    // all, which is what keeps every committed golden byte-identical.
    ir::IrModule module = make_pending_module();
    ir::Component plain;
    plain.id = "comp::Plain";
    plain.component_id = "82";
    plain.name = "Plain";
    plain.source = tessera::test::codegen_pending::span(200, 1, 1);
    plain.root = tessera::test::codegen_pending::leaf(ir::NodeKind::View, "node::1", "901", 210);
    module.components.push_back(std::move(plain));
    const auto result = generate_base(module);
    check(result.has_value(), "the mixed module generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    for (const codegen::GeneratedFile& file : *result) {
        if (file.path != "Plain.g.hpp") {
            continue;
        }
        check(file.text.find("tessera/ui/pending/") == std::string::npos,
              "a component without a pending surface includes no pending header");
        check(file.text.find("pending_") == std::string::npos,
              "a component without a pending surface emits no pending member");
    }
}

void test_goldens(bool update) {
    const auto result = generate_base(make_pending_module());
    check(result.has_value(), "the pending golden sample generates");
    if (!result) {
        return;
    }
    const std::string directory = std::string(TESSERA_CODEGEN_GOLDEN_DIR) + "/pending";
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
        check_eq(golden, file.text, file.path + " golden matches (codegen_pending_tests --write)");
    }
    if (update) {
        std::cout << "pending codegen goldens written to " << directory << "\n";
    }
}

void test_determinism() {
    const auto first = generate_base(make_pending_module());
    const auto second = generate_base(make_pending_module());
    check(first.has_value() && second.has_value(), "the pending sample generates twice");
    if (first && second) {
        check_eq(concatenated(*first), concatenated(*second), "two runs are byte-identical");
    }
}

void test_rejections() {
    const auto expect_error = [](const ir::IrModule& module, std::string_view what) {
        const auto result = generate_base(module);
        check(!result.has_value(), what);
        check(first_error(result).rfind("TESSERA_CODEGEN_", 0) == 0,
              std::string(what) + " reports a TESSERA_CODEGEN_ code");
    };
    {
        // A Suspense boundary without a fallback is rejected by the IR validator, and codegen must
        // not emit a swap it cannot fill.
        ir::IrModule module = make_pending_module();
        module.components[0].root.children[0].fallback.reset();
        expect_error(module, "a Suspense boundary without a fallback is rejected");
    }
    {
        ir::IrModule module = make_pending_module();
        module.components[0].root.children[0].children.clear();
        expect_error(module, "a Suspense boundary without a content subtree is rejected");
    }
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
    test_off_by_default();
    test_goldens(false);
    test_determinism();
    test_rejections();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending codegen tests passed\n";
    return 0;
}
