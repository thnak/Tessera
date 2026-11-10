// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/automation/snapshot.hpp"

#include <string>

namespace tessera::ui::automation {

// Canonical, stable JSON for one snapshot (docs/specs/automation-ids-v1.md sections 5 and 8):
// - one line, fixed field order, no insignificant whitespace, trailing newline;
// - `uiId`/`componentId`/`sequence` are decimal strings (JavaScript numbers lose precision
//   above 2^53, so a JSON number would corrupt them);
// - `parent` is a node index or null; `nodes` stays in preorder;
// - `bounds` values are fixed three-decimal numbers (rounded half away from zero and clamped to
//   +/-1e9) so the bytes do not depend on the platform's float formatting;
// - strings are escaped as JSON requires; non-ASCII UTF-8 bytes pass through unchanged.
[[nodiscard]] std::string snapshot_to_json(const UiSnapshot& snapshot);

} // namespace tessera::ui::automation
