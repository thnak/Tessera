// SPDX-License-Identifier: MIT

// `useResource` entries and Suspense coverage (docs/specs/pending-ui-v1.md sections 3.4, 4.2 and
// 9).

#include "tessera/compiler/ir/suspense_reads.hpp"
#include "validate_internal.hpp"

#include <string>
#include <string_view>
#include <unordered_set>

namespace tessera::compiler::ir {
namespace {

// A key is compared by value, so it is a scalar or a record of scalars: nothing that holds a
// collection, and nothing another resource's fetch produces (a dependent fetch would make the key
// itself pending).
constexpr std::size_t kMaxBoundaryReads = 8; // SuspenseBoundary::kMaxReads

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// Reports the first reason `expr` is not a scalar expression, or an empty string.
std::string scalar_problem(const ValidationContext& context, const ComponentScope& scope,
                           const Expr& expr) {
    switch (expr.kind) {
    case ExprKind::Array:
    case ExprKind::ListFilter:
    case ExprKind::ListSlice:
    case ExprKind::Record:
    case ExprKind::RecordUpdate:
        return "a key part is a scalar, not a list or a record";
    case ExprKind::Ref: {
        if (starts_with(expr.id, "res::")) {
            return "a key does not read another resource (`" + expr.id + "`)";
        }
        // A dotted ref is a record field, whose type is not tracked here; a bare ref has its
        // declared type, which must not be a list or a record.
        if (expr.id.find('.') == std::string::npos) {
            const auto declared = scope.declared_types.find(expr.id);
            if (declared != scope.declared_types.end()) {
                const std::string& type = declared->second;
                if (starts_with(type, "list<") || context.types.count(type) != 0) {
                    return "`" + expr.id + "` has type `" + type + "`, which is not a scalar";
                }
            }
        }
        return {};
    }
    default:
        break;
    }
    for (const ExprPtr& operand : expr.operands) {
        if (operand) {
            if (std::string problem = scalar_problem(context, scope, *operand); !problem.empty()) {
                return problem;
            }
        }
    }
    for (const TemplatePart& part : expr.parts) {
        if (part.expr) {
            if (std::string problem = scalar_problem(context, scope, *part.expr);
                !problem.empty()) {
                return problem;
            }
        }
    }
    return {};
}

void validate_key(ValidationContext& context, const ComponentScope& scope,
                  const Resource& resource) {
    const Expr& key = resource.key;
    const auto check_part = [&](const Expr& part, const std::string& what) {
        if (const std::string problem = scalar_problem(context, scope, part); !problem.empty()) {
            context.report("TESSERA_IR_UNSUPPORTED",
                           "the key of resource `" + resource.id + "`: " + what + problem,
                           resource.source);
            return;
        }
        validate_expr(context, scope, {}, &part, resource.source);
    };
    if (key.kind != ExprKind::Record) {
        check_part(key, "");
        return;
    }
    if (!key.type.empty()) {
        context.report("TESSERA_IR_UNSUPPORTED",
                       "the key of resource `" + resource.id +
                           "` is an anonymous record of scalars, not the declared record `" +
                           key.type + "`",
                       resource.source);
    }
    std::unordered_set<std::string> names;
    for (const RecordField& field : key.fields) {
        if (field.name.empty() || !names.insert(field.name).second) {
            context.report("TESSERA_IR_MALFORMED",
                           "the key of resource `" + resource.id +
                               "` has an empty or repeated field name",
                           resource.source);
        }
        if (!field.value) {
            context.report("TESSERA_IR_MALFORMED",
                           "the key of resource `" + resource.id + "` field `" + field.name +
                               "` has no value",
                           resource.source);
            continue;
        }
        check_part(*field.value, "field `" + field.name + "`: ");
    }
    if (key.fields.empty()) {
        context.report("TESSERA_IR_MALFORMED",
                       "the key of resource `" + resource.id + "` has no field", resource.source);
    }
}

} // namespace

void validate_resources(ValidationContext& context, const ComponentScope& scope,
                        const Component& component) {
    if (component.resources.empty()) {
        return;
    }
    for (const Resource& resource : component.resources) {
        validate_span(context, resource.source, "resource `" + resource.id + "`");
        bool declared_service = false;
        for (const Service& service : component.services) {
            declared_service = declared_service || service.id == resource.service;
        }
        if (!declared_service) {
            context.report("TESSERA_IR_UNRESOLVED",
                           "resource `" + resource.id + "` names service `" + resource.service +
                               "`, which this component does not declare",
                           resource.source);
        }
        if (!resource.type.empty() && resource.type != "int" && resource.type != "float" &&
            resource.type != "bool" && resource.type != "string") {
            context.report("TESSERA_IR_UNSUPPORTED",
                           "resource `" + resource.id + "` has data type `" + resource.type +
                               "`; V1 types a resource's data as int, float, bool or string",
                           resource.source);
        }
        if (resource.method.empty()) {
            context.report("TESSERA_IR_MALFORMED",
                           "resource `" + resource.id + "` names no service method",
                           resource.source);
        }
        validate_key(context, scope, resource);
    }
    const SuspenseReads reads = collect_suspense_reads(component);
    for (const std::size_t index : reads.uncovered) {
        const Resource& resource = component.resources[index];
        context.report("TESSERA_SUSPENSE_NO_BOUNDARY",
                       "suspending resource `" + resource.id +
                           "` is not read inside a Suspense boundary of this component, or is "
                           "read outside every boundary",
                       resource.source);
    }
    for (const BoundaryReads& boundary : reads.boundaries) {
        if (boundary.resources.size() > kMaxBoundaryReads) {
            context.report(
                "TESSERA_SUSPENSE_TOO_MANY_READS",
                "a Suspense boundary covers " + std::to_string(boundary.resources.size()) +
                    " suspending reads; the limit is " + std::to_string(kMaxBoundaryReads),
                boundary.boundary->source);
        }
    }
}

} // namespace tessera::compiler::ir
