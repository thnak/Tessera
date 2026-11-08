// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/selector.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

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
    [[nodiscard]] const std::map<std::string, std::string>& properties() const {
        return properties_;
    }

private:
    std::map<std::string, std::string> properties_;
};

ComputedStyle compute_style(const ComputedStyle* parent, const CascadeTable& cascade);

} // namespace tessera::ui::style
