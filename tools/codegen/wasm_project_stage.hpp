// SPDX-License-Identifier: MIT

#pragma once

#include "scene_project.hpp"

#include <string>
#include <string_view>
#include <vector>

// The project a browser module is previewing, staged one file at a time.
//
// A C ABI cannot take a project as an argument without a parser in the module or an array of
// pointers the page has to allocate in the Wasm heap. Staging avoids both: the page calls
// `tessera_project_begin`, then `tessera_project_add_source` / `tessera_project_add_stylesheet`
// once per file and `tessera_project_set_entry`, and every later export (`tessera_scene_json`,
// `tessera_canvas_apply`, `tessera_diagnostics_json`, ...) reads the staged project. Each string
// crosses the boundary once, as a plain `const char*` the JS glue already knows how to marshal.
//
// Each Wasm module holds its own stage (a module is its own program), so the page stages into the
// module it is about to call. A stage is a value: the strings are copied in, so the views
// `input()` returns stay valid until the next `begin()` or `add_*`.
namespace tessera::compiler::codegen::cli {

class ProjectStage {
public:
    void begin();
    void set_entry(std::string_view path);
    void add_source(std::string_view path, std::string_view text);
    void add_stylesheet(std::string_view path, std::string_view text);

    // True once an entry has been named. A stage with no entry previews nothing, and the
    // exports say so instead of choosing a file.
    [[nodiscard]] bool has_entry() const noexcept {
        return !entry_.empty();
    }
    // Views into this stage. Valid until the next mutation.
    [[nodiscard]] ProjectInput input() const;

private:
    struct Entry {
        std::string path;
        std::string text;
    };
    std::vector<Entry> sources_;
    std::vector<Entry> stylesheets_;
    std::string entry_;
    // Rebuilt by `input()`; a member so the returned spans have somewhere to live.
    mutable std::vector<ProjectFile> source_views_;
    mutable std::vector<ProjectFile> stylesheet_views_;
};

// The one stage of this module, which the shared exports in `wasm_project_exports.cpp` fill and the
// module's own exports read.
ProjectStage& project_stage();

} // namespace tessera::compiler::codegen::cli
