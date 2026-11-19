// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/selector.hpp"

#include <string>
#include <string_view>

// The selector list as text, reconstructed from the parsed form. Not installed: this is the
// engine's own fallback for a sheet that arrived as a blob, where the authored text is not carried
// (docs/specs/compiled-stylesheet-v1.md section 5).
//
// **Why reconstruct instead of storing.** The designer's `style.info` names the rule a developer
// would recognise (designer-protocol-v1 section 9.1), and a prepared sheet keeps that text
// (`PreparedItem::selector`). A blob does not, and adding it would bump the blob's `format_version`
// and regenerate every committed golden for a field only a design-time query reads. The parsed
// selector list is the whole meaning, so printing it back is exact for everything the engine can
// match - it is the same selector, spelled the way the parser normalises it (lowercased tag and
// pseudo names, one space around combinators).
namespace tessera::ui::style {

[[nodiscard]] std::string selector_list_text(const SelectorList& selectors);

} // namespace tessera::ui::style
