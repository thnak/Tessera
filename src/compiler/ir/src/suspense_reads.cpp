// SPDX-License-Identifier: MIT

// Which Suspense boundary covers which suspending resource (docs/specs/pending-ui-v1.md 4.2).

#include "tessera/compiler/ir/suspense_reads.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::compiler::ir {
namespace {

struct Collector {
    const Component& component;
    SuspenseReads result;
    std::vector<std::size_t> inside;  // per resource: `.data` reads inside some boundary
    std::vector<std::size_t> outside; // per resource: `.data` reads outside every boundary

    // The boundary the current node sits in, as an index into result.boundaries, or npos.
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    explicit Collector(const Component& source)
        : component(source),
          inside(source.resources.size(), 0),
          outside(source.resources.size(), 0) {}

    // A read of any member inside a boundary ties the resource to it (the part of the tree that
    // depends on the resource lives there); only `.data` read outside every boundary is an error,
    // because `.status` and `.error` are always readable.
    void read(std::string_view id, std::size_t boundary) {
        const std::size_t dot = id.find('.');
        if (dot == std::string_view::npos) {
            return;
        }
        const std::string_view base = id.substr(0, dot);
        const bool data = id.substr(dot + 1) == "data";
        for (std::size_t index = 0; index < component.resources.size(); ++index) {
            const Resource& resource = component.resources[index];
            if (resource.id != base || !resource.suspend) {
                continue;
            }
            if (boundary == kNone) {
                if (data) {
                    ++outside[index];
                }
                return;
            }
            ++inside[index];
            std::vector<std::size_t>& reads = result.boundaries[boundary].resources;
            if (std::find(reads.begin(), reads.end(), index) == reads.end()) {
                reads.push_back(index);
            }
            return;
        }
    }

    void expr(const Expr* value, std::size_t boundary) {
        if (value == nullptr) {
            return;
        }
        if (value->kind == ExprKind::Ref || value->kind == ExprKind::RecordUpdate) {
            read(value->id, boundary);
        }
        for (const ExprPtr& operand : value->operands) {
            expr(operand.get(), boundary);
        }
        for (const TemplatePart& part : value->parts) {
            expr(part.expr.get(), boundary);
        }
        for (const RecordField& field : value->fields) {
            expr(field.value.get(), boundary);
        }
    }

    void node(const Node& value, std::size_t boundary) {
        expr(value.key.get(), boundary);
        for (const auto& [name, prop] : value.props) {
            expr(prop.get(), boundary);
        }
        for (const auto& [name, style] : value.style) {
            expr(style.get(), boundary);
        }
        expr(value.text.get(), boundary);
        expr(value.provider_value.get(), boundary);
        expr(value.condition.get(), boundary);
        if (value.loop) {
            expr(value.loop->source.get(), boundary);
        }
        std::size_t inner = boundary;
        if (value.kind == NodeKind::Suspense) {
            inner = result.boundaries.size();
            result.boundaries.push_back(BoundaryReads{&value, {}});
        }
        for (const Node& child : value.children) {
            node(child, inner);
        }
        if (value.fallback) {
            node(*value.fallback, boundary); // the fallback is outside the boundary it replaces
        }
        if (value.item_template) {
            node(*value.item_template, boundary);
        }
    }
};

} // namespace

namespace {

bool reads_state(const Expr* expr, std::string_view target) {
    if (expr == nullptr) {
        return false;
    }
    if (expr->kind == ExprKind::Ref) {
        const std::string_view id = expr->id;
        if (id == target || (id.size() > target.size() && id.substr(0, target.size()) == target &&
                             id[target.size()] == '.')) {
            return true;
        }
    }
    for (const ExprPtr& operand : expr->operands) {
        if (reads_state(operand.get(), target)) {
            return true;
        }
    }
    for (const TemplatePart& part : expr->parts) {
        if (reads_state(part.expr.get(), target)) {
            return true;
        }
    }
    for (const RecordField& field : expr->fields) {
        if (reads_state(field.value.get(), target)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<std::size_t> resource_keyed_by(const Component& component,
                                             std::string_view state_id) {
    for (std::size_t index = 0; index < component.resources.size(); ++index) {
        if (reads_state(&component.resources[index].key, state_id)) {
            return index;
        }
    }
    return std::nullopt;
}

SuspenseReads collect_suspense_reads(const Component& component) {
    Collector collector(component);
    collector.node(component.root, Collector::kNone);
    for (std::size_t index = 0; index < component.resources.size(); ++index) {
        if (component.resources[index].suspend &&
            (collector.inside[index] == 0 || collector.outside[index] != 0)) {
            collector.result.uncovered.push_back(index);
        }
    }
    return std::move(collector.result);
}

} // namespace tessera::compiler::ir
