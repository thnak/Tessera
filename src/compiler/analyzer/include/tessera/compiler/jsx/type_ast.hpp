// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/source_map.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Type syntax of docs/specs/jsx-subset-v1.md section 5. Types are parsed so source looks like
// TypeScript and later lowering can pick IR types; nothing here type-checks.
namespace tessera::compiler::jsx {

struct TypeNode;
using TypePtr = std::unique_ptr<TypeNode>;

enum class TypeKind {
    Name,          // text = name (`int`, `Props`, `null`); children = generic arguments
    StringLiteral, // text = decoded contents
    Array,         // children = [element]
    Union,         // children = alternatives
    Object,        // members
    Function,      // param_names; children = parameter types followed by the return type
};

struct TypeMember {
    std::string name;
    bool optional = false;
    Span span;
    TypePtr type;
};

struct TypeNode {
    TypeKind kind = TypeKind::Name;
    Span span;
    std::uint32_t height = 1; // bounded by Limits::max_nesting_depth like every AST node
    std::string text;
    std::vector<TypePtr> children;
    std::vector<TypeMember> members;
    std::vector<std::string> param_names;
};

} // namespace tessera::compiler::jsx
