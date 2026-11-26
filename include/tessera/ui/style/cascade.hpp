// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/selector.hpp"
#include "tessera/ui/style/style_defaults.hpp"
#include "tessera/ui/style/style_environment.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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

    // The effective value: the entry this style holds itself, else the registry default it was
    // computed against. A pointer stays valid while any holder of the style lives.
    [[nodiscard]] const std::string* find(std::string_view property) const;
    [[nodiscard]] bool contains(std::string_view property) const;

    // The entries this style holds *itself*, sorted by property name (byte order): a delta over the
    // registry's initial values, not the ~100-entry effective style (issue #735, slice F). A copy
    // is one allocation instead of one per property, and a lookup is a binary search with no key
    // materialised (issue #711).
    using Entries = std::vector<std::pair<std::string, std::string>>;

    // One (name, value) pair of the effective style. Views, not copies: the name points into the
    // delta or the shared defaults table, both of which outlive the view.
    using Entry = std::pair<std::string_view, std::string_view>;

    // The effective style: this style's delta merged with the defaults it was computed against,
    // sorted by name - the iteration order `properties()` always gave. A view, so reading the
    // ~100-entry effective style allocates nothing and touches only the delta plus the defaults
    // table (issue #735, slice F).
    class Properties {
    public:
        class const_iterator {
        public:
            [[nodiscard]] const Entry& operator*() const noexcept {
                return current_;
            }
            [[nodiscard]] const Entry* operator->() const noexcept {
                return &current_;
            }
            const_iterator& operator++();
            [[nodiscard]] bool operator==(const const_iterator& other) const noexcept {
                return done_ == other.done_ && di_ == other.di_ && si_ == other.si_;
            }
            [[nodiscard]] bool operator!=(const const_iterator& other) const noexcept {
                return !(*this == other);
            }

        private:
            friend class Properties;
            const_iterator(const Entries* delta, const StyleDefaults* defaults, std::size_t di,
                           std::uint16_t si)
                : delta_(delta),
                  defaults_(defaults),
                  di_(di),
                  si_(si) {
                advance();
            }
            // Merges the two sorted ranges: the delta wins on a name it holds, and a default with
            // no initial value (absent until declared) is skipped.
            void advance();

            const Entries* delta_ = nullptr;
            const StyleDefaults* defaults_ = nullptr;
            std::size_t di_ = 0;
            std::uint16_t si_ = 0;
            Entry current_{};
            bool done_ = true;
        };

        Properties(const Entries* delta, const StyleDefaults* defaults)
            : delta_(delta),
              defaults_(defaults) {}

        [[nodiscard]] const_iterator begin() const {
            return const_iterator(delta_, defaults_, 0, 0);
        }
        [[nodiscard]] const_iterator end() const {
            // The exhausted position `advance` normalises to, so every exhausted iterator compares
            // equal to this one.
            return const_iterator(delta_, defaults_, delta_ != nullptr ? delta_->size() : 0,
                                  defaults_ != nullptr
                                      ? static_cast<std::uint16_t>(defaults_->size())
                                      : std::uint16_t{0});
        }
        [[nodiscard]] bool empty() const {
            return begin() == end();
        }
        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] bool operator==(const Properties& other) const;

    private:
        const Entries* delta_ = nullptr;
        const StyleDefaults* defaults_ = nullptr;
    };
    [[nodiscard]] Properties properties() const {
        return Properties(properties_ != nullptr ? &properties_->entries : &empty_entries(),
                          properties_ != nullptr ? properties_->defaults.get() : nullptr);
    }

    // The delta itself: only the entries this style holds, without the defaults. A writer that
    // rewrites values in place wants this - the defaults are literal CSS initial values, so they
    // can never contain a `var()` reference or need re-resolving.
    [[nodiscard]] const Entries& delta() const {
        return entries();
    }

    // The registry's initial values this style is a delta over. A style with none (a bare
    // `ComputedStyle`, as `compute_style` returns) holds every entry itself, which is what the
    // cascade tests pin. Attaching is idempotent and drops the cached digest, because the effective
    // style changed.
    void attach_defaults(std::shared_ptr<const StyleDefaults> defaults);

    // Two styles that share their entries are equal without comparing them; `equals` takes that
    // shortcut first. `identity` keeps the entries alive, so a holder can recognise "the very same
    // style as last time" without the address being reused by a different one.
    [[nodiscard]] bool same_entries(const ComputedStyle& other) const noexcept {
        return properties_ == other.properties_;
    }
    // Equality of the EFFECTIVE style. Two styles computed against the same defaults are equal
    // exactly when their deltas are equal, so this compares the deltas - a handful of entries -
    // instead of walking the ~100-entry merged view. That matters: `DiffingStyleProvider` calls
    // this once per node per frame, and a merged walk there cost 45% of a size-change frame
    // (issue #735). Styles with different (or absent) defaults fall back to the full comparison,
    // because their effective styles can differ even with equal deltas.
    [[nodiscard]] bool equals(const ComputedStyle& other) const {
        if (same_entries(other)) {
            return true;
        }
        const StyleDefaults* mine = properties_ != nullptr ? properties_->defaults.get() : nullptr;
        const StyleDefaults* theirs =
            other.properties_ != nullptr ? other.properties_->defaults.get() : nullptr;
        if (mine == theirs) {
            return entries() == other.entries();
        }
        return properties() == other.properties();
    }
    [[nodiscard]] std::shared_ptr<const void> identity() const {
        return properties_;
    }
    // `identity() == held` without the reference-count round trip a returned shared_ptr costs; the
    // memo walks compare once per node, so it adds up over a large tree.
    [[nodiscard]] bool has_identity(const std::shared_ptr<const void>& held) const noexcept {
        return properties_ == held;
    }

    // One cached 64-bit digest of the effective style, for a reader that folds the style into a
    // hash (the layout signature). It lives with the shared entries, so every copy of an unchanged
    // style
    // - the memo entry, the layout input - computes it once across frames; a mutation drops it.
    // Zero means "not computed" (a digest of zero is stored as one).
    [[nodiscard]] std::uint64_t cached_digest() const {
        return properties_ != nullptr ? properties_->digest.load(std::memory_order_relaxed) : 0;
    }
    void cache_digest(std::uint64_t digest) const {
        if (properties_ != nullptr) {
            properties_->digest.store(digest == 0 ? 1 : digest, std::memory_order_relaxed);
        }
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
    // The delta itself, for the operations that maintain it. `properties()` is the effective view.
    [[nodiscard]] const Entries& entries() const {
        return properties_ != nullptr ? properties_->entries : empty_entries();
    }
    [[nodiscard]] Entries::const_iterator lower_bound(std::string_view normalized) const;
    [[nodiscard]] static const Entries& empty_entries();
    // Copy-on-write: copying a style shares the entries (the layout input, the diff baseline and
    // every reader's copy used to deep-copy ~100 strings each, issue #711), and the first mutation
    // of a shared style clones them. A pointer `find` returned stays valid while any holder lives.
    [[nodiscard]] Entries& mutable_entries();

    struct Storage {
        Entries entries;
        // The initial values this delta is over. Shared, immutable and never reset by a mutation:
        // only the entries change.
        std::shared_ptr<const StyleDefaults> defaults;
        mutable std::atomic<std::uint64_t> digest{0};

        Storage() = default;
        explicit Storage(Entries values)
            : entries(std::move(values)) {}
    };
    std::shared_ptr<Storage> properties_;
};

// Computed-value time resolution of `var()` (stylesheet-v1 section 9): custom properties are
// resolved first (cycles become invalid), then every other property substitutes its references and
// falls back to the declared fallback. A reference that resolves to nothing follows `unset`
// semantics, so an inherited property keeps the parent's value and every other property is dropped
// for the registry's initial-value pass.
//
// `env()` (ADR-074) substitutes in the same pass, from `environment`; null is an environment with
// no variables, so every `env()` takes its fallback.
void resolve_variables(ComputedStyle& style, const ComputedStyle* parent,
                       std::vector<StyleDiagnostic>* diagnostics = nullptr,
                       const StyleEnvironment* environment = nullptr);

// Computed-value time resolution of `font-size` to `<n>px`: `em` and `%` are relative to the
// parent's font size, `rem` to the 16px root, `pt` is 4/3 px. Every later `em` (padding, margin,
// width, letter-spacing) then reads the element's own resolved size (issue #608). A value it does
// not understand (a keyword, `calc()`) is left as written.
void resolve_font_size(ComputedStyle& style, const ComputedStyle* parent);

ComputedStyle compute_style(const ComputedStyle* parent, const CascadeTable& cascade,
                            std::vector<StyleDiagnostic>* diagnostics = nullptr,
                            const StyleEnvironment* environment = nullptr);

} // namespace tessera::ui::style
