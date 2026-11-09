// SPDX-License-Identifier: MIT

// Lookups on LoweredHooks (docs/design/jsx-ir-lowering.md slice 6): the node lowering of slice 7
// resolves `ref={r}` and `onX={f}` through these instead of rebuilding the analysis.

#include "tessera/compiler/lowering/hook_lowering.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::lowering {
namespace {

template<typename Entry>
const Entry* find_named(const std::vector<Entry>& entries, std::string_view name) {
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const Entry& entry) { return entry.name == name; });
    return found == entries.end() ? nullptr : &*found;
}

} // namespace

const NamedRef* LoweredHooks::find_ref(std::string_view name) const {
    return find_named(ref_index, name);
}

const NamedRef* LoweredHooks::find_animated(std::string_view name) const {
    return find_named(animated_index, name);
}

const NamedSetter* LoweredHooks::find_setter(std::string_view name) const {
    const auto found = std::find_if(setter_index.begin(), setter_index.end(),
                                    [&](const NamedSetter& entry) { return entry.setter == name; });
    return found == setter_index.end() ? nullptr : &*found;
}

const HandlerRef* LoweredHooks::find_handler(std::string_view name) const {
    const auto found = std::find_if(handler_index.begin(), handler_index.end(),
                                    [&](const NamedHandler& entry) { return entry.name == name; });
    return found == handler_index.end() ? nullptr : &found->ref;
}

bool LoweredHooks::set_ref_target(std::string_view name, std::string node_id) {
    const NamedRef* named = find_ref(name);
    if (named == nullptr) {
        return false;
    }
    for (ir::Ref& ref : refs) {
        if (ref.id == named->id) {
            ref.target = std::move(node_id);
            return true;
        }
    }
    return false;
}

std::string LoweredHooks::add_handler(std::vector<ir::Param> params,
                                      std::vector<ir::Action> actions, ir::SourceSpan source) {
    ir::Handler handler;
    handler.id = "handler::" + std::to_string(next_handler_number);
    ++next_handler_number;
    handler.params = std::move(params);
    handler.actions = std::move(actions);
    handler.source = std::move(source);
    const std::string id = handler.id;
    handlers.push_back(std::move(handler));
    return id;
}

} // namespace tessera::compiler::lowering
