// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/selector.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {

// A recognised declaration that cannot be honoured is never silent (stylesheet-v1 section 10).
enum class StyleDiagnosticKind : std::uint8_t {
    UnsupportedProperty,
    // A recognised property whose value uses a form V1 does not implement (box-shadow `inset`,
    // 3D transform functions, SVG-only pointer-events keywords, user-select `all`/`contain`). The
    // declaration is kept; the engine ignores the unsupported part.
    UnsupportedValue,
    InvalidShorthand,
    InvalidAtRule,
    // A custom property participates in a var() dependency cycle. Cycle members are invalid at
    // computed-value time (TESSERA_CSS_VAR_CYCLE, stylesheet-v1 section 15).
    VarCycle,
    // A var() reference names no custom property and has no usable fallback
    // (TESSERA_CSS_UNRESOLVED_VAR, stylesheet-v1 section 15).
    UnresolvedVar,
    // A `::part(name)` rule names a part that a library component does not declare
    // (TESSERA_CSS_UNDECLARED_PART, stylesheet-v1 section 6.3). App components are not diagnosed:
    // only a library component publishes its part names (ADR-047 item 3).
    UndeclaredPart,
};

struct StyleDiagnostic {
    StyleDiagnosticKind kind = StyleDiagnosticKind::UnsupportedProperty;
    std::size_t offset = 0;
    std::string property;
    std::string message;
};

enum class Origin : std::uint8_t {
    UserAgent,
    User,
    Theme,
    Author,
    Inline,
};

inline constexpr std::uint32_t kUnlayeredLayer = UINT32_MAX;

struct CascadeDeclaration {
    std::string property;
    std::string value;
    bool important = false;
    Origin origin = Origin::Author;
    Specificity specificity;
    std::uint64_t source_order = 0;
    std::uint32_t layer = kUnlayeredLayer;
};

struct CascadeKey {
    Origin origin = Origin::Author;
    bool important = false;
    Specificity specificity;
    std::uint64_t source_order = 0;
    std::uint32_t layer = kUnlayeredLayer;
};

CascadeKey cascade_key(const CascadeDeclaration& declaration);
bool cascades_over(const CascadeKey& candidate, const CascadeKey& current);
bool cascades_over(const CascadeDeclaration& candidate, const CascadeDeclaration& current);

std::string normalize_property(std::string_view property);
bool is_inherited_property(std::string_view property);

class CascadeTable {
public:
    void add(CascadeDeclaration declaration);
    void add_declarations(const std::vector<Declaration>& declarations, Origin origin,
                          Specificity specificity, std::uint64_t& source_order,
                          std::uint32_t layer = kUnlayeredLayer);
    void clear();

    [[nodiscard]] const CascadeDeclaration* winner(std::string_view property) const;
    [[nodiscard]] std::vector<CascadeDeclaration> winners() const;
    [[nodiscard]] std::size_t size() const {
        return winners_.size();
    }

private:
    std::map<std::string, CascadeDeclaration> winners_;
};

class ComputedStyle {
public:
    void set(std::string property, std::string value);
    void erase(std::string_view property);

    [[nodiscard]] const std::string* find(std::string_view property) const;
    [[nodiscard]] bool contains(std::string_view property) const;
    // Property/value pairs sorted by property name (byte order), the iteration order the former
    // `std::map` gave. A flat vector: a copy is one allocation instead of one per property, and a
    // lookup is a binary search with no key materialised (issue #711).
    using Entries = std::vector<std::pair<std::string, std::string>>;
    [[nodiscard]] const Entries& properties() const {
        return properties_;
    }

    // Adds every `defaults` entry whose name this style does not already hold. `defaults` must be
    // sorted by name; one merge pass instead of a lookup and an insert per entry.
    void merge_missing(const Entries& defaults);

    // The same two operations for a caller that already holds a *normalized* name (lower case, the
    // form `normalize_property` produces). The registry's own longhand names are normalized by
    // construction, so the initial-value pass can skip the per-lookup `std::string` that
    // `normalize_property` would otherwise build (security review finding P1). A name that is not
    // normalized is simply not found, so this is only for names the registry owns.
    [[nodiscard]] const std::string* find_normalized(std::string_view normalized) const;
    [[nodiscard]] bool contains_normalized(std::string_view normalized) const;
    void set_normalized(std::string_view normalized, std::string value);

private:
    [[nodiscard]] Entries::const_iterator lower_bound(std::string_view normalized) const;

    Entries properties_;
};

// Computed-value time resolution of `var()` (stylesheet-v1 section 9): custom properties are
// resolved first (cycles become invalid), then every other property substitutes its references and
// falls back to the declared fallback. A reference that resolves to nothing follows `unset`
// semantics, so an inherited property keeps the parent's value and every other property is dropped
// for the registry's initial-value pass.
void resolve_variables(ComputedStyle& style, const ComputedStyle* parent,
                       std::vector<StyleDiagnostic>* diagnostics = nullptr);

// Computed-value time resolution of `font-size` to `<n>px`: `em` and `%` are relative to the
// parent's font size, `rem` to the 16px root, `pt` is 4/3 px. Every later `em` (padding, margin,
// width, letter-spacing) then reads the element's own resolved size (issue #608). A value it does
// not understand (a keyword, `calc()`) is left as written.
void resolve_font_size(ComputedStyle& style, const ComputedStyle* parent);

ComputedStyle compute_style(const ComputedStyle* parent, const CascadeTable& cascade,
                            std::vector<StyleDiagnostic>* diagnostics = nullptr);

} // namespace tessera::ui::style
