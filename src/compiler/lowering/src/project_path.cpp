// SPDX-License-Identifier: MIT

// Project path joining (docs/design/jsx-ir-lowering.md section 9, decision 13). The driver resolves
// imports with the same function, so file discovery and lowering cannot disagree about a path.

#include "tessera/compiler/lowering/node_lowering.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::lowering {

std::string join_project_path(std::string_view base, std::string_view relative) {
    std::string joined(base);
    if (!joined.empty()) {
        joined.push_back('/');
    }
    joined += relative;
    std::vector<std::string_view> parts;
    std::string_view pending = joined;
    std::size_t start = 0;
    while (start <= pending.size()) {
        const std::size_t slash = pending.find('/', start);
        const std::string_view part = pending.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    std::string out;
    for (const std::string_view part : parts) {
        if (!out.empty()) {
            out.push_back('/');
        }
        out += part;
    }
    return out;
}

} // namespace tessera::compiler::lowering
