// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The data types of a whole-project preview, in a header with no compiler or IR include so the
// stylesheet side of the preview (`scene_stylesheet.hpp`, also built into the diagnostics module)
// can name them without pulling the lowering in.
namespace tessera::compiler::codegen::cli {

// One file of a project the page holds in memory: its canonical project-relative POSIX path and its
// text. The views are borrowed for the duration of one call.
struct ProjectFile {
    std::string_view path;
    std::string_view text;
};

// A whole project, as the studio holds it: every source file, every stylesheet, and the entry the
// preview starts from (`app.entry` of tessera.json). Nothing here has a default: a project with no
// entry has nothing to preview, and `evaluate_project()` says so rather than choosing a file.
struct ProjectInput {
    std::span<const ProjectFile> sources;
    std::span<const ProjectFile> stylesheets;
    std::string_view entry;
    // The text of the project's `ui/services.json` (docs/specs/services-manifest-v1.md), or empty
    // when it has none. A project with no manifest rejects `useService<T>()`, so a page that
    // previews a project that uses services must stage it.
    std::string_view services_manifest{};
};

// A stylesheet the entry reaches, with the bytes it was given. `path` is canonical, so a diagnostic
// names the file the author edits.
struct SheetUse {
    std::string path;
    std::string text;
};

// A project by value, comparable with a borrowed one. A session that decides "is this the program I
// am already running?" keeps the last project's bytes, because the views of `ProjectInput` belong
// to the caller and are gone by the next call. Paths are compared as given: a caller passes one
// spelling for one file, and a different spelling is a different program to the ids it hashes.
struct OwnedProject {
    std::vector<std::pair<std::string, std::string>> sources;
    std::vector<std::pair<std::string, std::string>> stylesheets;
    std::string entry;
    std::string services_manifest;

    [[nodiscard]] static OwnedProject of(const ProjectInput& input) {
        OwnedProject out;
        for (const ProjectFile& file : input.sources) {
            out.sources.emplace_back(std::string(file.path), std::string(file.text));
        }
        for (const ProjectFile& file : input.stylesheets) {
            out.stylesheets.emplace_back(std::string(file.path), std::string(file.text));
        }
        out.entry = std::string(input.entry);
        out.services_manifest = std::string(input.services_manifest);
        return out;
    }

    [[nodiscard]] bool same_as(const ProjectInput& input) const {
        if (entry != input.entry || services_manifest != input.services_manifest ||
            sources.size() != input.sources.size() ||
            stylesheets.size() != input.stylesheets.size()) {
            return false;
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            if (sources[index].first != input.sources[index].path ||
                sources[index].second != input.sources[index].text) {
                return false;
            }
        }
        for (std::size_t index = 0; index < stylesheets.size(); ++index) {
            if (stylesheets[index].first != input.stylesheets[index].path ||
                stylesheets[index].second != input.stylesheets[index].text) {
                return false;
            }
        }
        return true;
    }
};

} // namespace tessera::compiler::codegen::cli
