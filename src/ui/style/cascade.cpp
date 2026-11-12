// SPDX-License-Identifier: MIT

#include "tessera/ui/style/cascade.hpp"

#include <algorithm>
#include <cctype>
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
    constexpr std::string_view kInherited[] = {
        "color",        "font",         "font-family",    "font-size",   "font-style",
        "font-weight",  "font-variant", "font-stretch",   "line-height", "letter-spacing",
        "word-spacing", "text-align",   "text-transform", "white-space", "visibility",
        "cursor",       "direction",    "writing-mode",
    };
    for (const std::string_view inherited : kInherited) {
        if (property == inherited) {
            return true;
        }
    }
    return property.starts_with("font-");
}

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
    properties_[normalize_property(property)] = std::move(value);
}

void ComputedStyle::erase(std::string_view property) {
    properties_.erase(normalize_property(property));
}

const std::string* ComputedStyle::find(std::string_view property) const {
    const std::string normalized = normalize_property(property);
    const auto found = properties_.find(normalized);
    return found == properties_.end() ? nullptr : &found->second;
}

bool ComputedStyle::contains(std::string_view property) const {
    return find(property) != nullptr;
}

ComputedStyle compute_style(const ComputedStyle* parent, const CascadeTable& cascade,
                            std::vector<StyleDiagnostic>* diagnostics) {
    ComputedStyle style;
    if (parent != nullptr) {
        for (const auto& [property, value] : parent->properties()) {
            if (is_inherited_property(property)) {
                style.set(property, value);
            }
        }
    }

    for (const CascadeDeclaration& declaration : cascade.winners()) {
        apply_keyword(style, declaration.property, declaration.value, parent);
    }
    // var() is resolved at computed-value time (stylesheet-v1 section 9), after inheritance and
    // the cascade so custom properties from both are visible to every dependent declaration.
    resolve_variables(style, parent, diagnostics);
    return style;
}

} // namespace tessera::ui::style
