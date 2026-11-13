// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

// The declared parts of one component (ADR-047 item 3): part names are the component's public
// styling API under SemVer, so a stylesheet that names a part the component does not declare is a
// mistake worth reporting.
struct ComponentParts {
    std::string name;               // the host-side type selector, for example `Button`
    std::vector<std::string> parts; // sorted and unique, so iteration order is deterministic
};

// The part declarations of the library components a stylesheet may target. App components are not
// registered: their parts are not a published contract, so an unknown part name is not diagnosed
// for them (stylesheet-v1 section 6.3, "a part name that is not declared by the component is a
// diagnostic only when the component is a library one").
class ComponentPartRegistry {
public:
    void declare(std::string name, std::span<const std::string_view> parts);
    void clear();

    [[nodiscard]] bool declares_component(std::string_view name) const;
    [[nodiscard]] bool declares_part(std::string_view component, std::string_view part) const;
    // The declaration of `name`, or nullptr when it is not a registered (library) component.
    [[nodiscard]] const ComponentParts* find(std::string_view name) const;
    [[nodiscard]] const std::vector<ComponentParts>& components() const {
        return components_;
    }

private:
    std::vector<ComponentParts> components_; // sorted by name
};

// One `StyleDiagnosticKind::UndeclaredPart` (TESSERA_CSS_UNDECLARED_PART) per `(component, part
// name)` whose `::part()` rule targets a registered component that does not declare that part. A
// selector whose host side is an unregistered type (an app component, or `*`) is never diagnosed.
[[nodiscard]] std::vector<StyleDiagnostic> check_part_selectors(const PreparedStyleSheet& prepared,
                                                                const ComponentPartRegistry& parts);

} // namespace tessera::ui::style
