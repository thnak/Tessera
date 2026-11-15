// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

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

    [[nodiscard]] const ir::Component& selected() const {
        return module.components[component];
    }
};

// Why a source produced no component. The preview reports failure *in* its document rather than
// beside it (P2's rule), so this exists for the callers that need the reason — the parity CLI's
// exit code and the session's build status — not for the page.
enum class EvaluateErrorCode {
    Parse,
    Analyze,
    Lower,
    Identity,
    NoComponent,
};

struct EvaluateError {
    EvaluateErrorCode code = EvaluateErrorCode::Parse;
    std::string detail;
};

// Evaluates `text` as the single file `file` and selects `component` (the first component when the
// name is empty). `salt` is the project salt the ids are hashed with; an empty salt leaves identity
// unassigned, so every `uiId` stays 0 — which is what an unsalted CLI run does.
[[nodiscard]] std::expected<EvaluatedScene, EvaluateError>
evaluate_source(std::string_view text, std::string_view file, std::string_view salt,
                std::string_view component);

// The module namespace a single in-memory file implies (a lowercased identifier-shaped file stem),
// shared so the snapshot and interaction artifacts agree on every string that reaches a document.
[[nodiscard]] std::string module_name_for(std::string_view file);

// A source path as the ids and the documents spell it: POSIX separators (automation-ids-v1 section
// 2.1), so a `\` from a Windows command line does not give a document no Linux run reproduces.
[[nodiscard]] std::string normalize_path(std::string_view file);

} // namespace tessera::compiler::codegen::cli
