// SPDX-License-Identifier: MIT

#pragma once

#include "scene_project.hpp"
#include "tessera/compiler/ir/module.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The front half of the in-memory preview, shared by every consumer that needs a lowered,
// identified component from a source string: the P2 snapshot document (`scene_preview.cpp`), the P4
// interaction session (`scene_interaction.cpp`) and the parity CLI. It is one implementation
// because the parity gate compares artifacts: a second copy of the stage order would be a second
// definition of what "the same source" means.
//
// The stages are the CLI's, in the CLI's order: parse -> analyse -> lower -> assign identity.
// Nothing here reads the filesystem, touches a clock or depends on the locale, so the same text
// produces the same module natively and in Wasm.
namespace tessera::compiler::codegen::cli {

struct EvaluatedScene {
    // Owns every string the IR and therefore the scene borrows; keep it alive with the session.
    ir::IrModule module;
    // Index of the selected component in `module.components`.
    std::size_t component = 0;
    // The stylesheets the program imports, in cascade order (the web's import-graph rule: a depth-
    // first walk from the entry, each sheet at its first import). Empty for a program that imports
    // no CSS, which is "no author CSS" and keeps the built-in sheet. A sheet that is present in the
    // project but never imported is NOT here: the preview applies what a native build would.
    std::vector<SheetUse> sheets;

    [[nodiscard]] const ir::Component& selected() const {
        return module.components[component];
    }
};

// Why a source produced no component. The preview reports failure *in* its document rather than
// beside it (P2's rule), so this exists for the callers that need the reason — the parity CLI's
// exit code and the session's build status — not for the page.
enum class EvaluateErrorCode : std::uint8_t {
    Parse,
    Analyze,
    Lower,
    Identity,
    NoComponent,
    // The project names an entry that is not one of its files, or names none.
    NoEntry,
    // A source file the entry reaches does not parse, analyse or lower, or an import names no file.
    // `EvaluateError::file` says which.
    File,
};

struct EvaluateError {
    EvaluateErrorCode code = EvaluateErrorCode::Parse;
    std::string detail;
    // The project file the failure is in, when it is in one; empty otherwise.
    std::string file;

    // A constructor rather than a third aggregate member: every pre-project caller names two
    // fields, and a defaulted aggregate member would trip -Wmissing-field-initializers on GCC.
    EvaluateError() = default;
    EvaluateError(EvaluateErrorCode failure, std::string reason, std::string in_file = {})
        : code(failure),
          detail(std::move(reason)),
          file(std::move(in_file)) {}
};

// Evaluates a whole project: parses the entry and every project file it reaches through relative
// imports, lowers them together, and selects `component` from the ENTRY file (the entry's first
// component when the name is empty). A file the entry never reaches is not parsed, so a half-typed
// unrelated file cannot blank the preview and a component name it declares cannot collide.
//
// `salt` is as for `evaluate_source()`. `EvaluatedScene::sheets` is the imported CSS, in cascade
// order, with the text taken from `input.stylesheets`.
[[nodiscard]] std::expected<EvaluatedScene, EvaluateError>
evaluate_project(const ProjectInput& input, std::string_view salt, std::string_view component);

// Evaluates `text` as the single file `file` and selects `component` (the first component when the
// name is empty). `salt` is the project salt the ids are hashed with; an empty salt leaves identity
// unassigned, so every `uiId` stays 0 — which is what an unsalted CLI run does.
//
// This is `evaluate_project()` over one source: `stylesheet` is a project file at `stylesheet_file`
// (issue #390), so a side-effect `import "./Preview.css";` resolves against it, and it is part of
// `EvaluatedScene::sheets` only when the source imports it. A source that does not import it
// previews with no author CSS, exactly as a native build of the same file would.
[[nodiscard]] std::expected<EvaluatedScene, EvaluateError>
evaluate_source(std::string_view text, std::string_view file, std::string_view salt,
                std::string_view component, std::string_view stylesheet = {},
                std::string_view stylesheet_file = {});

// The module namespace a single in-memory file implies (a lowercased identifier-shaped file stem),
// shared so the snapshot and interaction artifacts agree on every string that reaches a document.
[[nodiscard]] std::string module_name_for(std::string_view file);

// A source path as the ids and the documents spell it: POSIX separators (automation-ids-v1 section
// 2.1), so a `\` from a Windows command line does not give a document no Linux run reproduces.
[[nodiscard]] std::string normalize_path(std::string_view file);

} // namespace tessera::compiler::codegen::cli
