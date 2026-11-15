// SPDX-License-Identifier: MIT

#pragma once

#include "regenerate.hpp"

#include <string>
#include <string_view>
#include <vector>

// In-memory diagnostics: the browser's half of the preview (issue #165). The CLI's `run()` is
// file-driven by design (it reads `tessera.json`, follows imports, writes output), and a browser
// has no synchronous host filesystem, so the page cannot call it. This entry point runs the same
// stages in the same order over one in-memory source and returns the same document through the same
// serializer, which is what makes "the page shows what the native compiler reports" a testable
// invariant (scripts/ci/wasm_diagnostics_parity.py) rather than a claim.
namespace tessera::compiler::codegen::cli {

// Parses, analyses, lowers and validates `text` as the single file `file`, and returns the
// diagnostics document of docs/specs/compiler-diagnostics-v1.md. The browser passes a synthetic
// project-relative name (the page has one buffer, not a project); the span fields are byte offsets
// and 1-based line/column into `text`, so the page can underline without a filesystem.
//
// Deterministic: the document is a pure function of `text` and `file`, with no clock, no locale, no
// environment and no filesystem read.
[[nodiscard]] std::string diagnose_source(std::string_view text, std::string_view file);

} // namespace tessera::compiler::codegen::cli
