// SPDX-License-Identifier: MIT

#pragma once

#include "regenerate.hpp"
#include "regenerate_assets.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/module.hpp"

#include <optional>
#include <span>
#include <string>

// The output half of a codegen run: generate every file of a lowered module, compare it with the
// disk (write, `--check` or a dry run), and scaffold the developer classes that are missing. It
// lives in its own translation unit so `regenerate.cpp` keeps to the input half (read, parse,
// lower, validate, report) and stays inside the file size rules (AGENTS.md, Code conventions).
namespace tessera::compiler::codegen::cli::internal {

// Writes (or checks, or dry-runs) the output of `module` under `options.output_dir`, recording
// every message, generated entry, scaffolded path and the drift flag in `outcome`. `salt` is the
// resolved project salt; a non-empty one also emits `automation.manifest.json`. Reports through
// `outcome` only, so a caller's next step is always to return it.
void write_generated_output(const tessera::compiler::ir::IrModule& module,
                            std::span<const StylesheetEmbed> embeds,
                            const std::optional<EmbeddedTheme>& theme, const Options& options,
                            const std::string& salt, Outcome& outcome);

// The one-file emission modes (`--emit-ir`, `--emit-library-cpp`): exactly one file is written and
// no per-component C++ is generated, because the TSX sources stay the single source of truth (a
// source that does not lower cleanly stops the run instead of falling back to hand-built data).
// Reports through `outcome` only.
void write_emitted_file(const tessera::compiler::ir::IrModule& module, const Options& options,
                        Outcome& outcome);

} // namespace tessera::compiler::codegen::cli::internal
