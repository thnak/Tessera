// SPDX-License-Identifier: MIT

// ADR-056 slice 2: the emitted dirty mask. Shape and golden of the `Probe` component
// (codegen_mask_module.hpp); the behavior (which binding re-runs for which cell) is driven in
// codegen_mask_runtime_tests.cpp over the committed golden. `codegen_mask_tests --write` rewrites
// tests/golden/codegen/mask/Probe.g.hpp.

#include "codegen_mask_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef TESSERA_CODEGEN_MASK_GOLDEN_DIR
#error "TESSERA_CODEGEN_MASK_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string generate_probe() {
    const auto result =
        codegen::generate(tessera::test::codegen_mask::make_mask_module(), {}, std::nullopt);
    if (!result || result->size() != 1) {
        std::cerr << "  generate failed: " << (result ? "bad file count" : result.error().front())
                  << "\n";
        return {};
    }
    return result->front().text;
}

void test_shape(const std::string& text) {
    check(contains(text, "#include \"tessera/ui/scene/dirty_mask.hpp\""),
          "the base includes the runtime mask");
    check(contains(text, "tessera::ui::DirtyMask<7> dirty_ = tessera::ui::DirtyMask<7>::all();"),
          "one bit per cell plus the everything bit, every bit set until the first build");
    // Cells are sorted by id: prop::title, state::a, state::b, state::flag, state::items,
    // state::tone.
    check(contains(text, "static constexpr std::size_t kBit_prop_title_ = 0;") &&
              contains(text, "static constexpr std::size_t kBit_state_a_ = 1;") &&
              contains(text, "static constexpr std::size_t kBit_state_tone_ = 5;"),
          "the cell bits are public constants in sorted order");
    check(contains(text, "kDirty_text_2 = tessera::ui::DirtyMask<7>::of({1, 6});"),
          "a text binding reading one cell has a one-bit mask");
    check(contains(text, "kDirty_list_7 = tessera::ui::DirtyMask<7>::of({1, 4, 6});"),
          "the list item binding reads the list cell and the state its template names");
    check(contains(text, "kDirty_meta_5_className = tessera::ui::DirtyMask<7>::of({2, 6});"),
          "a metadata binding is keyed by its prop");
    check(contains(text, "kDirty_style_5_color = tessera::ui::DirtyMask<7>::of({5, 6});"),
          "a style binding is keyed by its property");
    check(contains(text, "kDirty_cond_6 = tessera::ui::DirtyMask<7>::of({3, 6});"),
          "a conditional binding reads its condition");
    check(contains(text, "dirty_.set(kBit_state_a_);"), "a setter sets its cell's bit");
    check(contains(text, "const auto consumed_dirty = dirty_;") &&
              contains(text, "dirty_.clear(consumed_dirty);"),
          "apply_state() consumes only the bits it saw");
    check(contains(text, "if (dirty_.intersects(kDirty_text_2))"),
          "a binding runs only when a cell it reads is dirty");
    check(contains(text, "mark_all_dirty();\n      flush_update();"),
          "a hand call of update() evaluates everything");
    check(contains(text, "->flush_update();"), "the scheduler ticket runs the dirty-only path");
}

void test_golden(bool update, const std::string& text) {
    const std::filesystem::path path =
        std::filesystem::path(TESSERA_CODEGEN_MASK_GOLDEN_DIR) / "Probe.g.hpp";
    if (update) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        return;
    }
    check(read_file(path) == text,
          "Probe.g.hpp matches its golden; run `codegen_mask_tests --write`");
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    check(ir::validate(tessera::test::codegen_mask::make_mask_module()).empty(),
          "the mask fixture is a valid IR module");
    const std::string text = generate_probe();
    check(!text.empty(), "the mask fixture generates");
    check(text == generate_probe(), "two runs are byte-identical");
    if (!text.empty()) {
        if (!update) {
            test_shape(text);
        }
        test_golden(update, text);
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen mask tests passed\n";
    return 0;
}
