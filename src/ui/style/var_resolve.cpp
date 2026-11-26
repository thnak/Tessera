// SPDX-License-Identifier: MIT

#include "color_functions.hpp"
#include "substitution.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/layout_digest.hpp"
#include "tessera/ui/style/style_environment.hpp"
#include "var_color.hpp"
#include "var_scan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {

namespace {

using detail::contains_var_function;
using detail::find_comma;
using detail::is_custom_property_name;
using detail::is_env_function;
using detail::is_var_function;
using detail::kMaxVarDepth;
using detail::match_paren;
using detail::normalize_env_name;
using detail::skip_string;
using detail::trim;

void collect_references(std::string_view value, std::vector<std::string>& out, int depth = 0) {
    if (depth > kMaxVarDepth) {
        return;
    }
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] == '"' || value[i] == '\'') {
            i = skip_string(value, i);
            continue;
        }
        const bool is_env = is_env_function(value, i);
        if (!is_var_function(value, i) && !is_env) {
            ++i;
            continue;
        }
        const std::size_t close = match_paren(value, i + 3);
        if (close == std::string_view::npos) {
            return;
        }
        const std::string_view inner = value.substr(i + 4, close - i - 4);
        const std::size_t comma = find_comma(inner);
        std::string name = trim(inner.substr(0, comma));
        if (!is_env && is_custom_property_name(name)) {
            out.push_back(std::move(name));
        }
        if (comma != std::string_view::npos) {
            // A fallback is part of the value: it may reference more custom properties.
            collect_references(inner.substr(comma + 1), out, depth + 1);
        }
        i = close + 1;
    }
}

struct CustomProperties {
    std::map<std::string, std::string> raw;
    std::map<std::string, std::optional<std::string>> resolved;
    std::set<std::string> invalid;
    std::vector<std::string> stack;
    std::map<std::string, std::uint8_t> visit; // 0 = unvisited, 1 = on stack, 2 = done
    const StyleEnvironment* environment = nullptr;
    // How many continuous `env()` variables (`is_continuous_environment_name`) substitution has
    // referenced, so a caller can tell whether a value it just resolved depends on one. A custom
    // property that did is remembered, because the property that uses it decides where it may go.
    int continuous_references = 0;
    std::set<std::string> continuous_custom;
};

void detect_cycle(CustomProperties& state, const std::string& name, int depth = 0) {
    if (depth > kMaxVarDepth) {
        // Too deep to be a real dependency graph: mark it invalid rather than recursing further.
        state.invalid.insert(name);
        return;
    }
    std::uint8_t& mark = state.visit[name];
    if (mark == 2) {
        return;
    }
    if (mark == 1) {
        // Back edge: every custom property from `name` to the stack top depends on itself.
        const auto first = std::find(state.stack.begin(), state.stack.end(), name);
        for (auto it = first; it != state.stack.end(); ++it) {
            state.invalid.insert(*it);
        }
        return;
    }
    mark = 1;
    state.stack.push_back(name);
    std::vector<std::string> references;
    collect_references(state.raw.at(name), references);
    for (const std::string& reference : references) {
        if (state.raw.count(reference) != 0) {
            detect_cycle(state, reference, depth + 1);
        }
    }
    state.stack.pop_back();
    mark = 2;
}

bool is_custom_property(std::string_view property) {
    return property.size() >= 2 && property[0] == '-' && property[1] == '-';
}

std::optional<std::string> resolve_custom(CustomProperties& state, const std::string& name,
                                          int depth);

std::optional<std::string> substitute(CustomProperties& state, std::string_view value, int depth) {
    if (depth > kMaxVarDepth) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] == '"' || value[i] == '\'') {
            const std::size_t end = skip_string(value, i);
            out.append(value.substr(i, end - i));
            i = end;
            continue;
        }
        const bool is_env = is_env_function(value, i);
        if (!is_var_function(value, i) && !is_env) {
            out.push_back(value[i]);
            ++i;
            continue;
        }
        const std::size_t close = match_paren(value, i + 3);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view inner = value.substr(i + 4, close - i - 4);
        const std::size_t comma = find_comma(inner);
        std::optional<std::string> replacement;
        if (is_env) {
            // A variable the host does not define, or an unknown name, "does not exist": the
            // fallback applies, and without one the declaration is invalid at computed-value time.
            const std::string name = normalize_env_name(inner.substr(0, comma));
            // Counted on reference, not on presence: whether a layout property is valid must not
            // depend on whether this device has a hinge sensor.
            if (is_continuous_environment_name(name)) {
                ++state.continuous_references;
            }
            if (state.environment != nullptr && !name.empty()) {
                if (const std::string* found = state.environment->find(name)) {
                    replacement = *found;
                }
            }
            if (!replacement && comma != std::string_view::npos) {
                // The padding around a fallback is not part of it: `rotate(env(x, 5deg))`, not
                // `rotate( 5deg)`.
                replacement = substitute(state, inner.substr(comma + 1), depth + 1);
                if (replacement) {
                    replacement = trim(*replacement);
                }
            }
        } else if (const std::string name = trim(inner.substr(0, comma));
                   is_custom_property_name(name)) {
            replacement = resolve_custom(state, name, depth + 1);
            if (!replacement && comma != std::string_view::npos) {
                replacement = substitute(state, inner.substr(comma + 1), depth + 1);
            }
        }
        if (!replacement) {
            return std::nullopt; // missing reference with no usable fallback
        }
        out.append(*replacement);
        i = close + 1;
    }
    return out;
}

std::optional<std::string> resolve_custom(CustomProperties& state, const std::string& name,
                                          int depth) {
    if (depth > kMaxVarDepth) {
        return std::nullopt;
    }
    if (state.invalid.count(name) != 0) {
        return std::nullopt;
    }
    const auto cached = state.resolved.find(name);
    if (cached != state.resolved.end()) {
        if (state.continuous_custom.count(name) != 0) {
            ++state.continuous_references;
        }
        return cached->second;
    }
    const auto raw = state.raw.find(name);
    if (raw == state.raw.end()) {
        return std::nullopt;
    }
    // Cycle members were rejected above, so a self-insert here cannot recurse; the entry still
    // guards against a malformed stream that slips past the dependency walk.
    state.resolved[name] = std::nullopt;
    // The depth is carried through, not restarted: a long acyclic chain is bounded by the same
    // budget as a nested fallback.
    const int references_before = state.continuous_references;
    std::optional<std::string> value = substitute(state, raw->second, depth);
    if (state.continuous_references != references_before) {
        state.continuous_custom.insert(name);
    }
    if (value) {
        // A substituted value is a token stream; store it in the canonical trimmed form so a
        // consumer never sees the padding around a fallback (or the whole property would be
        // stored as " 2px" instead of "2px").
        *value = trim(*value);
    }
    state.resolved[name] = value;
    return value;
}

} // namespace

void resolve_variables(ComputedStyle& style, const ComputedStyle* parent,
                       std::vector<StyleDiagnostic>* diagnostics,
                       const StyleEnvironment* environment) {
    // The delta, not the effective style: a registry default is a literal CSS initial value, so it
    // can never hold a `var()` reference and re-resolving it would be wasted work (issue #735).
    const auto& properties = style.delta();
    if (properties.empty()) {
        return;
    }

    // `light-dark()` and `color-mix()` are computed-value-time functions like `var()`: they read
    // the element's own `color-scheme` (inherited, so a nested dark region resolves its own
    // colours) and may appear inside a larger value. Resolve them after `var()` below, because a
    // custom property may hold the function, and before the initial-value pass, so a malformed call
    // drops the declaration exactly as an unresolvable `var()` does.
    const ColorContext color_context = detail::color_context_for(style, parent);

    // Style resolution runs for every dirty node, and almost no value references a custom property.
    // Scan first so the common case allocates nothing.
    bool has_custom_reference = false;
    bool has_property_reference = false;
    for (const auto& [property, value] : properties) {
        if (!contains_var_function(value)) {
            continue;
        }
        if (is_custom_property(property)) {
            has_custom_reference = true;
        } else {
            has_property_reference = true;
        }
    }
    if (!has_custom_reference && !has_property_reference) {
        // No `var()` anywhere, but a colour function may still be present: resolve it and stop.
        detail::resolve_color_functions(style, parent, color_context, diagnostics);
        return;
    }
    CustomProperties state;
    state.environment = environment;
    for (const auto& [property, value] : properties) {
        if (is_custom_property(property)) {
            state.raw.emplace(property, value);
        }
    }

    if (!state.raw.empty()) {
        for (const auto& [name, value] : state.raw) {
            (void)value;
            detect_cycle(state, name);
        }
        for (const auto& [name, value] : state.raw) {
            (void)value;
            resolve_custom(state, name, 0);
        }
        if (diagnostics != nullptr) {
            for (const auto& [name, value] : state.raw) {
                (void)value;
                if (state.invalid.count(name) != 0) {
                    diagnostics->push_back(
                        StyleDiagnostic{StyleDiagnosticKind::VarCycle, 0, name,
                                        "custom property takes part in a var() dependency cycle"});
                }
            }
        }
    }

    struct Update {
        std::string property;
        std::optional<std::string> value;
    };
    std::vector<Update> updates;

    for (const auto& [property, value] : properties) {
        if (is_custom_property(property)) {
            if (state.invalid.count(property) != 0) {
                // Cycle members get one VarCycle diagnostic each; they are invalid, not unset.
                updates.push_back(Update{property, std::nullopt});
                continue;
            }
            const auto resolved = state.resolved.find(property);
            if (resolved == state.resolved.end() || !resolved->second) {
                if (diagnostics != nullptr) {
                    diagnostics->push_back(StyleDiagnostic{
                        StyleDiagnosticKind::UnresolvedVar, 0, property,
                        "custom property references a var() with no value and no usable fallback"});
                }
                updates.push_back(Update{property, std::nullopt});
            } else if (*resolved->second != value) {
                updates.push_back(Update{property, *resolved->second});
            }
            continue;
        }

        if (!contains_var_function(value)) {
            continue;
        }
        bool reported = false;
        const int references_before = state.continuous_references;
        std::optional<std::string> resolved = substitute(state, value, 0);
        const bool reads_continuous = state.continuous_references != references_before;
        if (reads_continuous && is_layout_property(property)) {
            // A hinge angle must never be a layout input (ADR-074 section 4): the declaration is
            // invalid at computed-value time, which is `unset`.
            resolved = std::nullopt;
            reported = true;
            if (diagnostics != nullptr) {
                diagnostics->push_back(StyleDiagnostic{
                    StyleDiagnosticKind::UnresolvedVar, 0, property,
                    "a continuous env() variable (--tessera-hinge-angle) may feed only a "
                    "paint-only property, not a layout property"});
            }
        }
        if (resolved) {
            *resolved = trim(*resolved);
            if (resolved->empty()) {
                // An empty token stream is not a valid declaration value, so the property is
                // invalid at computed-value time like an unresolvable reference.
                resolved = std::nullopt;
            }
        }
        if (!resolved) {
            if (diagnostics != nullptr && !reported) {
                diagnostics->push_back(
                    StyleDiagnostic{StyleDiagnosticKind::UnresolvedVar, 0, property,
                                    "var() or env() reference has no usable value"});
            }
            // Invalid at computed-value time behaves like `unset`: an inherited property keeps the
            // parent's value, everything else is dropped so the initial-value pass applies.
            if (is_inherited_property(property) && parent != nullptr) {
                if (const std::string* inherited = parent->find(property); inherited != nullptr) {
                    updates.push_back(Update{property, *inherited});
                    continue;
                }
            }
            updates.push_back(Update{property, std::nullopt});
            continue;
        }
        if (*resolved != value) {
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

    detail::resolve_color_functions(style, parent, color_context, diagnostics);
}
} // namespace tessera::ui::style
