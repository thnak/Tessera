// SPDX-License-Identifier: MIT

#include "wasm_project_stage.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {

void ProjectStage::begin() {
    sources_.clear();
    stylesheets_.clear();
    entry_.clear();
}

void ProjectStage::set_entry(std::string_view path) {
    entry_ = std::string(path);
}

void ProjectStage::add_source(std::string_view path, std::string_view text) {
    // A path staged twice is the later bytes: the page re-stages the file it edited, and two
    // entries for one path would make `lower_project` refuse the project as a duplicate.
    for (Entry& known : sources_) {
        if (known.path == path) {
            known.text = std::string(text);
            return;
        }
    }
    sources_.push_back(Entry{std::string(path), std::string(text)});
}

void ProjectStage::add_stylesheet(std::string_view path, std::string_view text) {
    for (Entry& known : stylesheets_) {
        if (known.path == path) {
            known.text = std::string(text);
            return;
        }
    }
    stylesheets_.push_back(Entry{std::string(path), std::string(text)});
}

ProjectInput ProjectStage::input() const {
    source_views_.clear();
    stylesheet_views_.clear();
    for (const Entry& entry : sources_) {
        source_views_.push_back(ProjectFile{entry.path, entry.text});
    }
    for (const Entry& entry : stylesheets_) {
        stylesheet_views_.push_back(ProjectFile{entry.path, entry.text});
    }
    return ProjectInput{source_views_, stylesheet_views_, entry_};
}

ProjectStage& project_stage() {
    static ProjectStage stage;
    return stage;
}

} // namespace tessera::compiler::codegen::cli
