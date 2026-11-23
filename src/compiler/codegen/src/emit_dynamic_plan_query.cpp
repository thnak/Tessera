// SPDX-License-Identifier: MIT

// The query half of `DynamicPlan` (docs/design/compiler.md section 11). Split from
// `emit_dynamic_plan.cpp`, which is the walk that fills the plan.

#include "emit_dynamic.hpp"

#include <string_view>

namespace tessera::compiler::codegen::internal {

const DynamicGroup* DynamicPlan::group(unsigned parent_ordinal) const {
    for (const DynamicGroup& group : groups) {
        if (group.parent_ordinal == parent_ordinal) {
            return &group;
        }
    }
    return nullptr;
}

const ListTemplate* DynamicPlan::list(unsigned ordinal) const {
    for (const ListTemplate& list : lists) {
        if (list.ordinal == ordinal) {
            return &list;
        }
    }
    return nullptr;
}

const ListTemplate* DynamicPlan::virtual_collection(unsigned container_ordinal) const {
    for (const ListTemplate& list : lists) {
        if (list.is_virtual && list.container_ordinal == container_ordinal) {
            return &list;
        }
    }
    return nullptr;
}

DynamicChildKind DynamicPlan::child_kind(unsigned parent_ordinal, unsigned child_ordinal) const {
    const DynamicGroup* owning = group(parent_ordinal);
    if (owning == nullptr) {
        return DynamicChildKind::Static;
    }
    for (const DynamicChild& child : owning->children) {
        if (child.kind != DynamicChildKind::Static && child.ordinal == child_ordinal) {
            return child.kind;
        }
    }
    return DynamicChildKind::Static;
}

std::string branch_member(unsigned ordinal, std::string_view suffix) {
    return "branch_" + std::to_string(ordinal) + "_" + std::string(suffix);
}

} // namespace tessera::compiler::codegen::internal
