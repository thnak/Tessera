// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Issue #463 (docs/design/css-isolation.md, "Inspector and tools"): a CSS module scopes a class to
// `card_0b82a5` so the selector index never confuses two files' `.card`, but the person reading a
// node tree must not have to reverse a hash to find their own file. The IR keeps both names per
// module class (`StylesheetClass`); this is the lookup an inspector, the devtools tree or
// click-to-source uses to show the authored name beside the file that declares it.
namespace tessera::compiler::ir {

// One class of a node as an inspector shows it. `file` is empty for a class no module declares
// (a plain global class, shown as written).
struct AuthoredClass {
    std::string name; // the authored `local` name for a module class, else the class as written
    std::string file; // project-relative path of the declaring `*.module.css`, else empty
    bool scoped = false;
};

// The authored name and declaring file of one scoped class, or nullopt when no module sheet maps
// it. Scoped names are unique within a project (`TESSERA_CSS_MODULE_HASH_COLLISION` rejects two
// files that hash alike), so a hit is never ambiguous.
[[nodiscard]] std::optional<AuthoredClass>
find_authored_class(std::span<const StylesheetRef> sheets, std::string_view scoped);

// A node's whole `className` as an inspector shows it, in order: each space-separated token is
// resolved with `find_authored_class` and otherwise kept verbatim.
[[nodiscard]] std::vector<AuthoredClass> authored_classes(std::span<const StylesheetRef> sheets,
                                                          std::string_view class_name);

} // namespace tessera::compiler::ir
