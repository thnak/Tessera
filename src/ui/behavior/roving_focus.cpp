// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/roving_focus.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

#include <vector>

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `orientation`, `wrap`, `typeahead`.
constexpr std::string_view kOptionNames[] = {"orientation", "wrap", "typeahead"};

std::string_view option_string(const ArgsView& args, std::string_view name,
                               std::string_view fallback) {
    const ArgValue* value = args.find(name, kOptionNames);
    if (value == nullptr || value->kind != ArgValue::Kind::String) {
        return fallback;
    }
    return value->text;
}

bool option_bool(const ArgsView& args, std::string_view name, bool fallback) {
    const ArgValue* value = args.find(name, kOptionNames);
    if (value == nullptr || value->kind != ArgValue::Kind::Bool) {
        return fallback;
    }
    return value->boolean;
}

Node::FocusGroup group_for(std::string_view orientation) {
    if (orientation == "horizontal") {
        return Node::FocusGroup::Horizontal;
    }
    if (orientation == "vertical") {
        return Node::FocusGroup::Vertical;
    }
    return Node::FocusGroup::Both;
}

// The focusable descendants of `node`, in tree order, skipping invisible subtrees. This is the same
// order `FocusManager::move_in_focus_group` walks, so an index this behavior reports and a move the
// manager makes agree.
void collect_items(Node& node, std::vector<Node*>& items) {
    if (!node.rendered()) {
        return;
    }
    if (node.focusable()) {
        items.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_items(*child, items);
    }
}

} // namespace

void RovingFocus::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    if (!targets.all_attached() || targets.nodes.empty()) {
        return;
    }
    target_ = targets.nodes.front();
    router_ = services.router;
    apply(option_string(args, "orientation", "vertical"));
    // `typeahead` is a flag on the group, not a behavior of its own: the manager's `typeahead()`
    // reads it, so the hook only has to set it.
    if (target_ != nullptr) {
        target_->set_typeahead(option_bool(args, "typeahead", false));
    }
}

void RovingFocus::update(const ArgsView& args) {
    if (target_ == nullptr) {
        return;
    }
    apply(option_string(args, "orientation", "vertical"));
    target_->set_typeahead(option_bool(args, "typeahead", false));
}

void RovingFocus::detach() {
    // A group that outlives its component would keep a Tab stop in a subtree that is about to be
    // destroyed, so detach always clears what it set.
    if (target_ != nullptr && applied_) {
        target_->set_focus_group(Node::FocusGroup::None);
        target_->set_typeahead(false);
    }
    target_ = nullptr;
    router_ = nullptr;
    applied_ = false;
    outputs_ = {};
}

std::span<const OutputValue> RovingFocus::outputs() const {
    return outputs_;
}

void RovingFocus::apply(std::string_view orientation) {
    if (target_ == nullptr) {
        return;
    }
    target_->set_focus_group(group_for(orientation));
    applied_ = true;
    outputs_[0].kind = OutputValue::Kind::Number;
    outputs_[0].number = static_cast<double>(active_index());
}

int RovingFocus::active_index() const {
    if (target_ == nullptr || router_ == nullptr) {
        return -1;
    }
    Node* const focused = router_->focus_manager().focused();
    if (focused == nullptr) {
        return -1;
    }
    std::vector<Node*> items;
    collect_items(*target_, items);
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index] == focused) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

} // namespace tessera::ui::behavior
