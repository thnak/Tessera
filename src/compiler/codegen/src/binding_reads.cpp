// SPDX-License-Identifier: MIT

// The component half of the binding-read analysis (ADR-056 slice 1, issue #441): walks one
// component's tree and records, per dynamic binding, the cells it reads. Pre-order ordinals match
// the other collectors (every node counts, children first, then a virtual collection's item
// template).

#include "binding_reads.hpp"
#include "binding_reads_collector.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// The prop a controlled input paints from: `value`, else `defaultValue` (interpolation.cpp). Empty
// for any other node.
std::string paint_prop_name(const ir::Node& node) {
    if (node.kind != ir::NodeKind::TextInput && node.kind != ir::NodeKind::TextArea) {
        return {};
    }
    return node.props.find("value") != node.props.end() ? "value" : "defaultValue";
}

bool is_metadata_prop(const std::string& name) {
    return name == "id" || name == "className" || name == "role" || starts_with(name, "aria-");
}

// One list template being walked. Every expression inside its subtree reads through `items`, so the
// per-item content is a single binding that depends on the list cell and whatever else the template
// names (loop locals excluded).
struct Template {
    unsigned ordinal = 0;
    std::vector<std::string> locals;
    std::optional<ReadCollector> items;
};

struct Walk {
    const ir::Component& component;
    BindingReadTable& table;
    unsigned ordinal = 0;

    void put(BindingKind kind, unsigned at, std::string name, ReadSet reads) {
        table.entries.push_back(
            BindingReads{BindingKey{kind, at, std::move(name)}, std::move(reads)});
    }

    [[nodiscard]] ReadSet reads_of(const ir::Expr* expr) const {
        ReadCollector collector(component, {});
        collector.add(expr);
        return collector.finish();
    }

    void node(const ir::Node& node, Template* enclosing);
    void node_bindings(const ir::Node& node, unsigned self);
    static void template_reads(const ir::Node& node, ReadCollector& items);
};

void Walk::node_bindings(const ir::Node& node, unsigned self) {
    if (node.condition) {
        put(BindingKind::Condition, self, {}, reads_of(node.condition.get()));
    }
    const std::string paint = paint_prop_name(node);
    if (node.text) {
        put(BindingKind::Text, self, {}, reads_of(node.text.get()));
    } else if (!paint.empty()) {
        // A controlled input's `value`/`defaultValue` is the text apply_state() keeps in sync.
        put(BindingKind::Text, self, {}, reads_of(node.props.at(paint).get()));
    }
    for (const auto& [name, value] : node.props) {
        if (!node.text && !paint.empty() && name == paint) {
            continue; // already recorded as the node's Text binding
        }
        BindingKind kind = BindingKind::Attribute;
        if (node.kind == ir::NodeKind::Component) {
            kind = BindingKind::InstanceProp;
        } else if (is_metadata_prop(name)) {
            kind = BindingKind::Metadata;
        }
        put(kind, self, name, reads_of(value.get()));
    }
    for (const auto& [name, value] : node.style) {
        put(BindingKind::Style, self, name, reads_of(value.get()));
    }
}

// Inside a template the node's expressions are not bindings of their own; they join the template's.
void Walk::template_reads(const ir::Node& node, ReadCollector& items) {
    if (node.condition) {
        items.add(node.condition.get());
    }
    if (node.text) {
        items.add(node.text.get());
    }
    if (node.key) {
        items.add(node.key.get());
    }
    for (const auto& entry : node.props) {
        items.add(entry.second.get());
    }
    for (const auto& entry : node.style) {
        items.add(entry.second.get());
    }
}

void Walk::node(const ir::Node& node, Template* enclosing) {
    const unsigned self = ++ordinal;

    Template started;
    Template* active = enclosing;
    if (node.loop) {
        if (enclosing != nullptr) {
            // A list inside a list template is deferred by the plan; if it ever appears, assume the
            // worst rather than guess which of the two sources the inner content follows.
            enclosing->items->add(nullptr);
        } else {
            started.ordinal = self;
            started.locals.push_back(node.loop->item);
            if (!node.loop->index.empty()) {
                started.locals.push_back(node.loop->index);
            }
            started.items.emplace(component, started.locals);
            put(BindingKind::ListSource, self, {}, reads_of(node.loop->source.get()));
            started.items->add(node.loop->source.get());
            active = &started;
        }
    }

    if (active != nullptr) {
        template_reads(node, *active->items);
    } else {
        node_bindings(node, self);
    }

    for (const ir::Node& child : node.children) {
        this->node(child, active);
    }
    if (node.item_template) {
        this->node(*node.item_template, active);
    }

    if (active == &started) {
        put(BindingKind::ListItem, started.ordinal, {}, started.items->finish());
    }
}

} // namespace

const ReadSet* BindingReadTable::find(BindingKind kind, unsigned ordinal,
                                      std::string_view name) const {
    const BindingKey key{kind, ordinal, std::string(name)};
    const auto it = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const BindingReads& entry, const BindingKey& wanted) { return entry.key < wanted; });
    if (it == entries.end() || it->key != key) {
        return nullptr;
    }
    return &it->reads;
}

BindingReadTable analyze_binding_reads(const ir::Component& component) {
    BindingReadTable table;
    Walk walk{component, table};
    walk.node(component.root, nullptr);

    for (std::size_t index = 0; index < component.effects.size(); ++index) {
        const ir::Effect& effect = component.effects[index];
        ReadCollector inputs(component, {});
        for (const std::string& dep : effect.deps) {
            inputs.add_ref_id(dep);
        }
        walk.put(BindingKind::Effect, static_cast<unsigned>(index), effect.id, inputs.finish());
    }

    std::sort(table.entries.begin(), table.entries.end(),
              [](const BindingReads& a, const BindingReads& b) { return a.key < b.key; });
    return table;
}

} // namespace tessera::compiler::codegen::internal
