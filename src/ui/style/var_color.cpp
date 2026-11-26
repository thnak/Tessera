// SPDX-License-Identifier: MIT

#include "var_color.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>

namespace tessera::ui::style::detail {

// The scheme a `light-dark()` in this element resolves against: the element's own computed
// `color-scheme` when it is `light` or `dark`, else the parent's (the property is inherited, so the
// parent's value is already in `style` unless this element overrode it), else light. `normal` and
// `only light`/`only dark` are read for their light/dark half; the `only` prefix only suppresses
// the UA's own scheme switching, which the engine does not do.
ColorContext color_context_for(const ComputedStyle& style, const ComputedStyle* parent) {
    const auto read = [](const std::string* value) -> std::optional<bool> {
        if (value == nullptr) {
            return std::nullopt;
        }
        std::string text(*value);
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (text == "dark") {
            return true;
        }
        if (text == "light") {
            return false;
        }
        return std::nullopt;
    };
    if (const std::optional<bool> own = read(style.find("color-scheme"))) {
        return ColorContext{*own};
    }
    if (parent != nullptr) {
        if (const std::optional<bool> inherited = read(parent->find("color-scheme"))) {
            return ColorContext{*inherited};
        }
    }
    return ColorContext{};
}

// Substitutes `light-dark()`/`color-mix()` in every non-custom property. A malformed call is
// invalid at computed-value time, which behaves like `unset`: an inherited property keeps the
// parent's value and everything else is dropped so the initial-value pass applies.
void resolve_color_functions(ComputedStyle& style, const ComputedStyle* parent,
                             const ColorContext& context,
                             std::vector<StyleDiagnostic>* diagnostics) {
    struct Update {
        std::string property;
        std::optional<std::string> value;
    };
    std::vector<Update> updates;
    for (const auto& [property, value] : style.delta()) {
        if (property.starts_with("--") || value.find('(') == std::string::npos) {
            continue;
        }
        std::string resolved = value;
        if (!detail::substitute_color_functions(resolved, context)) {
            if (diagnostics != nullptr) {
                diagnostics->push_back(StyleDiagnostic{
                    StyleDiagnosticKind::UnsupportedValue, 0, property,
                    "light-dark()/color-mix() call is malformed or uses an unsupported "
                    "interpolation space"});
            }
            if (is_inherited_property(property) && parent != nullptr) {
                if (const std::string* inherited = parent->find(property); inherited != nullptr) {
                    updates.push_back(Update{property, *inherited});
                    continue;
                }
            }
            updates.push_back(Update{property, std::nullopt});
            continue;
        }
        if (resolved != value) {
            updates.push_back(Update{property, std::move(resolved)});
        }
    }
    for (Update& update : updates) {
        if (update.value) {
            style.set(std::move(update.property), std::move(*update.value));
        } else {
            style.erase(update.property);
        }
    }
}

} // namespace tessera::ui::style::detail
