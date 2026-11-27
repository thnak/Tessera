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

    void read(std::string_view id, std::size_t boundary) {
        constexpr std::string_view kSuffix = ".data";
        if (id.size() <= kSuffix.size() || id.substr(id.size() - kSuffix.size()) != kSuffix) {
            return;
        }
        const std::string_view base = id.substr(0, id.size() - kSuffix.size());
        for (std::size_t index = 0; index < component.resources.size(); ++index) {
            const Resource& resource = component.resources[index];
            if (resource.id != base || !resource.suspend) {
                continue;
            }
            if (boundary == kNone) {
                ++outside[index];
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
