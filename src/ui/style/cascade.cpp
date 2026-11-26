// SPDX-License-Identifier: MIT

#include "tessera/ui/style/cascade.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace tessera::ui::style {
namespace {

int origin_rank(Origin origin, bool important) {
    if (important) {
        switch (origin) {
        case Origin::UserAgent:
            return 5;
        case Origin::User:
            return 4;
        case Origin::Theme:
            return 3;
        case Origin::Inline:
            return 2;
        case Origin::Author:
            return 1;
        }
    } else {
        switch (origin) {
        case Origin::Inline:
            return 5;
        case Origin::Author:
            return 4;
        case Origin::Theme:
            return 3;
        case Origin::User:
            return 2;
        case Origin::UserAgent:
            return 1;
        }
    }
    return 0;
}

bool is_keyword(std::string_view value, std::string_view keyword) {
    if (value.size() != keyword.size()) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(keyword[i]))) {
            return false;
        }
    }
    return true;
}

bool is_unset_keyword(std::string_view value) {
    return is_keyword(value, "unset") || is_keyword(value, "revert") ||
           is_keyword(value, "revert-layer");
}

void apply_keyword(ComputedStyle& style, std::string_view property, std::string_view value,
                   const ComputedStyle* parent) {
    if (is_keyword(value, "inherit")) {
        if (parent != nullptr) {
            if (const std::string* inherited = parent->find(property); inherited != nullptr) {
                style.set(std::string(property), *inherited);
                return;
            }
        }
        style.erase(property);
        return;
    }
    if (is_keyword(value, "initial")) {
        style.erase(property);
        return;
    }
    if (is_unset_keyword(value)) {
        if (is_inherited_property(property) && parent != nullptr) {
            if (const std::string* inherited = parent->find(property); inherited != nullptr) {
                style.set(std::string(property), *inherited);
                return;
            }
        }
        style.erase(property);
        return;
    }
    style.set(std::string(property), std::string(value));
}

} // namespace

CascadeKey cascade_key(const CascadeDeclaration& declaration) {
    return CascadeKey{declaration.origin, declaration.important, declaration.specificity,
                      declaration.source_order, declaration.layer};
}

bool cascades_over(const CascadeKey& candidate, const CascadeKey& current) {
    if (candidate.important != current.important) {
        return candidate.important;
    }
    const int candidate_rank = origin_rank(candidate.origin, candidate.important);
    const int current_rank = origin_rank(current.origin, current.important);
    if (candidate_rank != current_rank) {
        return candidate_rank > current_rank;
    }

    if (candidate.layer != current.layer) {
        if (candidate.important) {
            if (candidate.layer == kUnlayeredLayer) {
                return false;
            }
            if (current.layer == kUnlayeredLayer) {
                return true;
            }
            return candidate.layer < current.layer;
        }
        if (candidate.layer == kUnlayeredLayer) {
            return true;
        }
        if (current.layer == kUnlayeredLayer) {
            return false;
        }
        return candidate.layer > current.layer;
    }

    if (candidate.specificity != current.specificity) {
        return candidate.specificity > current.specificity;
    }
    return candidate.source_order > current.source_order;
}

bool cascades_over(const CascadeDeclaration& candidate, const CascadeDeclaration& current) {
    return cascades_over(cascade_key(candidate), cascade_key(current));
}

std::string normalize_property(std::string_view property) {
    if (property.size() >= 2 && property[0] == '-' && property[1] == '-') {
        return std::string(property);
    }
    std::string result(property);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool is_inherited_property(std::string_view property) {
    if (property.size() >= 2 && property[0] == '-' && property[1] == '-') {
        return true;
    }
    // Sorted by byte value so the lookup is a binary search rather than a linear scan: this runs
    // once per property of every parent style, i.e. ~120 times per node per style pass (security
    // review finding P1). `static_assert` below keeps the order honest.
    constexpr std::string_view kInherited[] = {
        "color",
        "color-scheme",
        "cursor",
        "direction",
        "font",
        "font-family",
        "font-size",
        "font-stretch",
        "font-style",
        "font-variant",
        "font-weight",
        "letter-spacing",
        "line-height",
        "list-style-position",
        "list-style-type",
        "pointer-events",
        "text-align",
        "text-decoration-color",
        "text-decoration-line",
        "text-decoration-style",
        "text-decoration-thickness",
        "text-transform",
        "user-select",
        "visibility",
        "white-space",
        "word-spacing",
        "writing-mode",
    };
    const auto found = std::lower_bound(std::begin(kInherited), std::end(kInherited), property);
    if (found != std::end(kInherited) && *found == property) {
        return true;
    }
    return property.starts_with("font-");
}

namespace {

// `kInherited` must stay sorted for the binary search above. A new inherited property inserted in
// the wrong place would silently stop inheriting, so the order is a compile-time invariant.
constexpr bool inherited_list_is_sorted() {
    constexpr std::string_view kList[] = {
        "color",
        "color-scheme",
        "cursor",
        "direction",
        "font",
        "font-family",
        "font-size",
        "font-stretch",
        "font-style",
        "font-variant",
        "font-weight",
        "letter-spacing",
        "line-height",
        "list-style-position",
        "list-style-type",
        "pointer-events",
        "text-align",
        "text-decoration-color",
        "text-decoration-line",
        "text-decoration-style",
        "text-decoration-thickness",
        "text-transform",
        "user-select",
        "visibility",
        "white-space",
        "word-spacing",
        "writing-mode",
    };
    for (std::size_t i = 1; i < std::size(kList); ++i) {
        if (!(kList[i - 1] < kList[i])) {
            return false;
        }
    }
    return true;
}

static_assert(inherited_list_is_sorted(), "kInherited must stay sorted for binary search");

} // namespace

void CascadeTable::add(CascadeDeclaration declaration) {
    const std::string property = normalize_property(declaration.property);
    declaration.property = property;
    const auto found = winners_.find(property);
    if (found == winners_.end() || cascades_over(declaration, found->second)) {
        winners_[property] = std::move(declaration);
    }
}

void CascadeTable::add_declarations(const std::vector<Declaration>& declarations, Origin origin,
                                    Specificity specificity, std::uint64_t& source_order,
                                    std::uint32_t layer) {
    for (const Declaration& declaration : declarations) {
        add(CascadeDeclaration{declaration.property, declaration.value, declaration.important,
                               origin, specificity, source_order, layer});
        ++source_order;
    }
}

void CascadeTable::clear() {
    winners_.clear();
}

const CascadeDeclaration* CascadeTable::winner(std::string_view property) const {
    const std::string normalized = normalize_property(property);
    const auto found = winners_.find(normalized);
    return found == winners_.end() ? nullptr : &found->second;
}

std::vector<CascadeDeclaration> CascadeTable::winners() const {
    std::vector<CascadeDeclaration> result;
    result.reserve(winners_.size());
    for (const auto& [property, declaration] : winners_) {
        (void)property;
        result.push_back(declaration);
    }
    return result;
}

void ComputedStyle::set(std::string property, std::string value) {
    set_normalized(normalize_property(property), std::move(value));
}

const ComputedStyle::Entries& ComputedStyle::empty_entries() {
    static const Entries kEmpty;
    return kEmpty;
}

ComputedStyle::Entries& ComputedStyle::mutable_entries() {
    if (properties_ == nullptr) {
        properties_ = std::make_shared<Storage>();
    } else if (properties_.use_count() > 1) {
        // The clone is a delta over the same defaults: they are immutable and shared, so the copy
        // keeps the pointer rather than dropping it.
        std::shared_ptr<const StyleDefaults> defaults = properties_->defaults;
        properties_ = std::make_shared<Storage>(properties_->entries);
        properties_->defaults = std::move(defaults);
    } else {
        properties_->digest.store(0, std::memory_order_relaxed);
    }
    return properties_->entries;
}

void ComputedStyle::attach_defaults(std::shared_ptr<const StyleDefaults> defaults) {
    if (properties_ == nullptr) {
        if (defaults == nullptr) {
            return;
        }
        properties_ = std::make_shared<Storage>();
    } else if (properties_.use_count() > 1) {
        std::shared_ptr<const StyleDefaults> previous = properties_->defaults;
        properties_ = std::make_shared<Storage>(properties_->entries);
        properties_->defaults = std::move(previous);
    }
    properties_->defaults = std::move(defaults);
    // The effective style changed even though the delta did not, so a digest folded from the old
    // defaults is stale.
    properties_->digest.store(0, std::memory_order_relaxed);
}

void ComputedStyle::erase(std::string_view property) {
    const std::string normalized = normalize_property(property);
    const auto found = lower_bound(normalized);
    if (found != entries().end() && found->first == normalized) {
        const auto index = found - entries().cbegin();
        Entries& mutable_values = mutable_entries();
        mutable_values.erase(mutable_values.begin() + index);
    }
}

ComputedStyle::Entries::const_iterator
ComputedStyle::lower_bound(std::string_view normalized) const {
    const Entries& values = entries();
    // Names mostly differ in their first byte, so decide on it before calling into a full compare.
    const auto less = [](std::string_view left, std::string_view right) {
        if (!left.empty() && !right.empty() && left.front() != right.front()) {
            return static_cast<unsigned char>(left.front()) <
                   static_cast<unsigned char>(right.front());
        }
        return left < right;
    };
    auto first = values.begin();
    auto count = values.end() - first;
    while (count > 0) {
        const auto half = count / 2;
        const auto middle = first + half;
        if (less(middle->first, normalized)) {
            first = middle + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    return first;
}

const std::string* ComputedStyle::find(std::string_view property) const {
    // Property names are almost always already lower case; only materialise a copy when not.
    const bool lower = std::none_of(property.begin(), property.end(),
                                    [](unsigned char c) { return c >= 'A' && c <= 'Z'; });
    if (lower) {
        return find_normalized(property);
    }
    return find_normalized(normalize_property(property));
}

bool ComputedStyle::contains(std::string_view property) const {
    // The delta only: `initial`/`unset` leave a property to its default, and a caller asking
    // whether this style *holds* the entry must not see the default as one.
    const std::string normalized = normalize_property(property);
    const auto found = lower_bound(normalized);
    return found != entries().end() && found->first == normalized;
}

const std::string* ComputedStyle::find_normalized(std::string_view normalized) const {
    const auto found = lower_bound(normalized);
    if (found != entries().end() && found->first == normalized) {
        return &found->second;
    }
    // Not held: the registry default it was computed against, if any.
    return properties_ != nullptr && properties_->defaults != nullptr
               ? properties_->defaults->find(normalized)
               : nullptr;
}

bool ComputedStyle::contains_normalized(std::string_view normalized) const {
    const auto found = lower_bound(normalized);
    return found != entries().end() && found->first == normalized;
}

void ComputedStyle::set_normalized(std::string_view normalized, std::string value) {
    // The delta never stores a value equal to its default: the effective style is the same either
    // way, and this is what keeps a node's style a handful of entries instead of ~100 (slice F).
    const std::string* fallback = properties_ != nullptr && properties_->defaults != nullptr
                                      ? properties_->defaults->find(normalized)
                                      : nullptr;
    const bool redundant = fallback != nullptr && *fallback == value;
    // Computed styles are mostly built in name order (inherited properties, then the initial
    // values), so appending is the common case.
    const Entries& current = entries();
    if (current.empty() || std::string_view(current.back().first) < normalized) {
        if (!redundant) {
            mutable_entries().emplace_back(std::string(normalized), std::move(value));
        }
        return;
    }
    const auto found = lower_bound(normalized);
    const auto index = found - current.cbegin();
    const bool exists = found != current.end() && found->first == normalized;
    if (redundant) {
        if (exists) {
            Entries& mutable_values = mutable_entries();
            mutable_values.erase(mutable_values.begin() + index);
        }
        return;
    }
    Entries& mutable_values = mutable_entries();
    if (exists) {
        mutable_values[static_cast<std::size_t>(index)].second = std::move(value);
        return;
    }
    mutable_values.emplace(mutable_values.begin() + index, std::string(normalized),
                           std::move(value));
}

void ComputedStyle::merge_missing(const Entries& defaults) {
    // Entries may be shared with another style: move strings out only when this style owns them.
    const bool owned = properties_ != nullptr && properties_.use_count() == 1;
    const Entries& mine_entries = entries();
    Entries merged;
    merged.reserve(mine_entries.size() + defaults.size());
    auto mine = mine_entries.begin();
    const auto take = [&](const Entries::value_type& entry) {
        if (owned) {
            merged.push_back(std::move(const_cast<Entries::value_type&>(entry)));
        } else {
            merged.push_back(entry);
        }
    };
    for (const auto& entry : defaults) {
        while (mine != mine_entries.end() && mine->first < entry.first) {
            take(*mine++);
        }
        if (mine != mine_entries.end() && mine->first == entry.first) {
            take(*mine++);
        } else {
            merged.push_back(entry);
        }
    }
    for (; mine != mine_entries.end(); ++mine) {
        take(*mine);
    }
    std::shared_ptr<const StyleDefaults> defaults_table =
        properties_ != nullptr ? properties_->defaults : nullptr;
    properties_ = std::make_shared<Storage>(std::move(merged));
    properties_->defaults = std::move(defaults_table);
}

ComputedStyle compute_style(const ComputedStyle* parent, const CascadeTable& cascade,
                            std::vector<StyleDiagnostic>* diagnostics,
                            const StyleEnvironment* environment) {
    ComputedStyle style;
    if (parent != nullptr) {
        // The effective style: an inherited property the parent left at its default still inherits
        // that default, and a child that overrides it must see the value it is overriding.
        for (const auto& [property, value] : parent->properties()) {
            if (is_inherited_property(property)) {
                style.set_normalized(property, std::string(value));
            }
        }
    }

    for (const CascadeDeclaration& declaration : cascade.winners()) {
        apply_keyword(style, declaration.property, declaration.value, parent);
    }
    // var() is resolved at computed-value time (stylesheet-v1 section 9), after inheritance and
    // the cascade so custom properties from both are visible to every dependent declaration.
    resolve_variables(style, parent, diagnostics, environment);
    resolve_font_size(style, parent);
    return style;
}

} // namespace tessera::ui::style
