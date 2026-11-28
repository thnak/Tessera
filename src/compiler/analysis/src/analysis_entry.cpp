// SPDX-License-Identifier: MIT

#include "analysis_entry.h"
#include "tessera/compiler/analysis/language.hpp"
#include "tessera/compiler/analysis/project_index.hpp"
#include "tessera/compiler/analysis/symbol_json.hpp"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace tessera::compiler::analysis;

struct State {
    ProjectIndex index;
    std::vector<SourceFile> staged_files;
    std::vector<TextEdit> staged_edits;
};

State& state() {
    static State instance;
    return instance;
}

char* duplicate_out(const std::string& text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

std::string text_of(const char* text) {
    return text == nullptr ? std::string() : std::string(text);
}

} // namespace

extern "C" {

void tessera_analysis_reset() {
    state() = State{};
}

void tessera_analysis_set_file(int file, const char* path, const char* text) {
    state().staged_files.push_back(
        SourceFile{static_cast<FileId>(file), text_of(path), text_of(text)});
}

void tessera_analysis_commit_snapshot() {
    State& s = state();
    s.staged_edits.clear();
    s.index = ProjectIndex::build(std::move(s.staged_files));
    s.staged_files.clear();
}

void tessera_analysis_edit(int file, int offset, int length, const char* new_text) {
    state().staged_edits.push_back(TextEdit{static_cast<FileId>(file),
                                            static_cast<std::uint32_t>(offset),
                                            static_cast<std::uint32_t>(length), text_of(new_text)});
}

int tessera_analysis_commit_edits() {
    State& s = state();
    const bool ok = s.index.apply_edits(s.staged_edits);
    s.staged_edits.clear();
    return ok ? 1 : 0;
}

const char* tessera_analysis_query_json(const char* type, int file) {
    const std::string kind = text_of(type);
    const std::optional<FileId> only =
        file < 0 ? std::nullopt : std::optional<FileId>(static_cast<FileId>(file));
    if (kind == "symbols") {
        return duplicate_out(symbols_to_json(state().index, only));
    }
    if (kind == "diagnostics") {
        return duplicate_out(diagnostics_to_json(state().index, only));
    }
    if (kind == "capabilities") {
        return duplicate_out(analysis_capabilities_json());
    }
    return nullptr;
}

const char* tessera_analysis_request_json(const char* type, int file, int offset, int flag) {
    if (file < 0 || offset < 0) {
        return nullptr;
    }
    const std::string kind = text_of(type);
    const auto id = static_cast<FileId>(file);
    const auto at = static_cast<std::uint32_t>(offset);
    if (kind == "definition") {
        return duplicate_out(definition_to_json(state().index, id, at));
    }
    if (kind == "references") {
        return duplicate_out(references_to_json(state().index, id, at, flag != 0));
    }
    if (kind == "hover") {
        return duplicate_out(hover_to_json(state().index, id, at));
    }
    return nullptr;
}

void tessera_free(const char* text) {
    std::free(const_cast<char*>(text));
}

} // extern "C"
