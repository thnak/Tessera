// SPDX-License-Identifier: MIT

#include "tessera/compiler/ir/authored_class.hpp"

#include <cstddef>
#include <utility>

namespace tessera::compiler::ir {

std::optional<AuthoredClass> find_authored_class(std::span<const StylesheetRef> sheets,
                                                 std::string_view scoped) {
    for (const StylesheetRef& sheet : sheets) {
        for (const StylesheetClass& item : sheet.classes) {
            if (item.scoped == scoped) {
                return AuthoredClass{item.local, sheet.path, true};
            }
        }
    }
    return std::nullopt;
}

std::vector<AuthoredClass> authored_classes(std::span<const StylesheetRef> sheets,
                                            std::string_view class_name) {
    std::vector<AuthoredClass> out;
    std::size_t index = 0;
    while (index < class_name.size()) {
        while (index < class_name.size() && class_name[index] == ' ') {
            ++index;
        }
        const std::size_t start = index;
        while (index < class_name.size() && class_name[index] != ' ') {
            ++index;
        }
        if (start == index) {
            break;
        }
        const std::string_view token = class_name.substr(start, index - start);
        if (std::optional<AuthoredClass> found = find_authored_class(sheets, token)) {
            out.push_back(std::move(*found));
        } else {
            out.push_back(AuthoredClass{std::string(token), {}, false});
        }
    }
    return out;
}

} // namespace tessera::compiler::ir
