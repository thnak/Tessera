// SPDX-License-Identifier: MIT

// ADR-056 slices 2 and 3: the emitted dirty mask and the scheduled children. Shape and goldens of
// the `Probe` fixture (one component, one binding of each kind) and of the `Top` -> `Mid` -> `Leaf`
// chain (codegen_mask_module.hpp); the behavior is driven in codegen_mask_runtime_tests.cpp and
// codegen_chain_runtime_tests.cpp over the committed goldens. `codegen_mask_tests --write` rewrites
// tests/golden/codegen/mask/*.g.hpp.

#include "codegen_mask_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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

// Every generated file of a module by path; empty on failure.
std::map<std::string, std::string> generate_files(const ir::IrModule& module) {
    std::map<std::string, std::string> files;
    const auto result = codegen::generate(module, {}, std::nullopt);
    if (!result) {
        std::cerr << "  generate failed: " << result.error().front() << "\n";
        return files;
    }
    for (const codegen::GeneratedFile& file : *result) {
        files[file.path] = file.text;
    }
    return files;
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
          "a text binding reading one cell has a one-cell mask");
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

// ADR-056 decision 3: a written child prop marks its bit on the child and schedules the child's own
// ticket; the owner never calls the child's update().
void test_chain_shape(const std::string& top, const std::string& mid) {
    check(contains(top, "child_3_.note_dirty(Mid::kBit_prop_label_);") &&
              contains(top, "this->child_3_.schedule_update();"),
          "a written child prop marks its bit on the child and schedules the child's ticket");
    check(!contains(top, "child_3_.update();") && !contains(mid, "child_3_.update();"),
          "a parent never calls a child's update()");
}

void test_golden(bool update, const std::string& name, const std::string& text) {
    const std::filesystem::path path =
        std::filesystem::path(TESSERA_CODEGEN_MASK_GOLDEN_DIR) / name;
    if (update) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        return;
    }
    check(read_file(path) == text, name + " matches its golden; run `codegen_mask_tests --write`");
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    const ir::IrModule probe_module = tessera::test::codegen_mask::make_mask_module();
    const ir::IrModule chain_module = tessera::test::codegen_mask::make_chain_module();
    check(ir::validate(probe_module).empty(), "the mask fixture is a valid IR module");
    check(ir::validate(chain_module).empty(), "the chain fixture is a valid IR module");
    const auto probe = generate_files(probe_module);
    const auto chain = generate_files(chain_module);
    check(probe.size() == 1 && chain.size() == 3, "the fixtures generate one base per component");
    check(probe == generate_files(probe_module) && chain == generate_files(chain_module),
          "two runs are byte-identical");
    if (probe.size() == 1 && chain.size() == 3) {
        if (!update) {
            test_shape(probe.at("Probe.g.hpp"));
            test_chain_shape(chain.at("Top.g.hpp"), chain.at("Mid.g.hpp"));
        }
        for (const auto* files : {&probe, &chain}) {
            for (const auto& [name, text] : *files) {
                test_golden(update, name, text);
            }
        }
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen mask tests passed\n";
    return 0;
}
