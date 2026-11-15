// SPDX-License-Identifier: MIT

#include "a11y_role_traits.hpp"
#include "a11y_text.hpp"
#include "tessera/ui/accessibility/a11y_tree.hpp"

namespace tessera::ui::a11y {

std::string_view to_string(Role role) {
    return detail::role_traits(role).name;
}

Role role_from_string(std::string_view name) {
    const std::string_view value = detail::trim(name);
    for (std::size_t index = 0; index < detail::kRoleTable.size(); ++index) {
        if (detail::equal_ci(value, detail::kRoleTable[index].name)) {
            return static_cast<Role>(index);
        }
    }
    for (const detail::RoleAlias& alias : detail::kRoleAliases) {
        if (detail::equal_ci(value, alias.name)) {
            return alias.role;
        }
    }
    return Role::Generic;
}

} // namespace tessera::ui::a11y
