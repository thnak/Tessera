// SPDX-License-Identifier: MIT

// Event payload/event-name tables, generated dispatch-adapters and their emission.

#include "emit_events.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::internal {
namespace {

constexpr PayloadRecord kPayloads[] = {
    {"PointerEvent", "PointerEventInfo", "pointer_event_info"},
    {"ClickEvent", "ClickEvent", "click_event"},
    {"KeyEvent", "KeyEventInfo", "key_event_info"},
    {"FocusEvent", "FocusEventInfo", "focus_event_info"},
    {"TextChangeEvent", "TextChangeEvent", "text_change_event"},
    {"TextSubmitEvent", "TextSubmitEvent", "text_submit_event"},
    {"TextSelection", "TextSelection", "text_selection_event"},
    {"CompositionEvent", "CompositionEvent", "composition_event"},
    {"ScrollEvent", "ScrollEventInfo", "scroll_event_info"},
    {"ErrorInfo", "ErrorInfo", "error_info"},
};

struct EventTypeMapping {
    std::string_view jsx_name;
    std::string_view enum_name;
};

// The JSX event names the scene can dispatch today. onClick maps to the synthesized Click event
// (same-target pointer press/release, Enter/Space on a focused button, accessibility invoke);
// onPointerUp stays the raw release. onChange/onSubmit map to EditText (submit synthesis is later).
constexpr EventTypeMapping kEventTypes[] = {
    {"onClick", "Click"},
    {"onPointerDown", "PointerDown"},
    {"onPointerUp", "PointerUp"},
    {"onPointerMove", "PointerMove"},
    {"onPointerOver", "PointerOver"},
    {"onPointerOut", "PointerOut"},
    {"onPointerEnter", "PointerEnter"},
    {"onPointerLeave", "PointerLeave"},
    {"onKeyDown", "KeyDown"},
    {"onKeyUp", "KeyUp"},
    {"onFocus", "Focus"},
    {"onBlur", "Blur"},
    {"onChange", "EditText"},
    {"onSubmit", "EditText"},
};

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

const EventTypeMapping* find_event_type(std::string_view jsx_name) {
    for (const EventTypeMapping& mapping : kEventTypes) {
        if (mapping.jsx_name == jsx_name) {
            return &mapping;
        }
    }
    return nullptr;
}

std::string adapter_function_name(unsigned ordinal, std::string_view jsx_name) {
    std::string name = "event_adapter_" + std::to_string(ordinal) + "_";
    for (const char c : jsx_name) {
        name += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
    }
    return name;
}

const ir::Handler* find_handler(const ir::Component& component, std::string_view id) {
    for (const ir::Handler& handler : component.handlers) {
        if (handler.id == id) {
            return &handler;
        }
    }
    return nullptr;
}

const ir::Binding* find_binding(const std::vector<const ir::Binding*>& bindings,
                                std::string_view id) {
    for (const ir::Binding* binding : bindings) {
        if (binding->id == id) {
            return binding;
        }
    }
    return nullptr;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

} // namespace

const PayloadRecord* find_payload(std::string_view record) {
    for (const PayloadRecord& payload : kPayloads) {
        if (payload.record == record) {
            return &payload;
        }
    }
    return nullptr;
}

std::string handler_type(std::string_view ir_type) {
    if (const PayloadRecord* payload = find_payload(ir_type)) {
        return "tessera::ui::" + std::string(payload->cpp_type);
    }
    const std::optional<std::string> mapped = cpp_type(ir_type);
    return mapped ? *mapped : std::string();
}

std::string method_name(std::string_view id) {
    std::string out;
    for (const char c : id) {
        if (c == ':') {
            if (out.empty() || out.back() != '_') {
                out += '_';
            }
            continue;
        }
        out += c;
    }
    return out;
}

std::optional<std::string> payload_signature(const std::vector<ir::Param>& params, bool& typed,
                                             std::string& param_name) {
    typed = false;
    param_name = "event";
    if (params.empty()) {
        return std::string("const tessera::ui::Event& event");
    }
    const PayloadRecord* payload = find_payload(params.front().type);
    if (payload == nullptr) {
        return std::nullopt;
    }
    typed = true;
    if (!params.front().name.empty()) {
        param_name = params.front().name;
    }
    return "const tessera::ui::" + std::string(payload->cpp_type) + "& " + param_name;
}

bool collect_event_adapters(const ir::Node& node, const ir::Component& component,
                            const std::vector<const ir::Binding*>& bindings,
                            const std::vector<ControlledInput>& controlled_inputs,
                            unsigned& ordinal, std::vector<EventAdapter>& adapters,
                            std::string& error) {
    ++ordinal;
    const auto controlled =
        std::any_of(controlled_inputs.begin(), controlled_inputs.end(),
                    [&](const ControlledInput& input) { return input.ordinal == ordinal; });
    // A Component node's `events` are callback props (docs/specs/component-model-v1.md section 3):
    // apply_props() binds them into the instance's generated callable, so they are not scene
    // events.
    const bool callback_props = node.kind == ir::NodeKind::Component;
    const bool virtual_collection =
        node.kind == ir::NodeKind::VirtualList || node.kind == ir::NodeKind::VirtualGrid;
    for (const auto& [jsx_name, target] : node.events) {
        if (callback_props) {
            continue;
        }
        if (virtual_collection && (jsx_name == "onScroll" || jsx_name == "onEndReached")) {
            // The collection-level events are driven by the generated scroll entry point
            // (emit_virtual_scroll.cpp), not through a scene EventType adapter.
            continue;
        }
        if (controlled && jsx_name == "onChange") {
            // Bound through TextChangeBinding by emit_controlled_wiring, not the generic adapter.
            continue;
        }
        const EventTypeMapping* mapping = find_event_type(jsx_name);
        if (mapping == nullptr) {
            return fail(error, "event `" + jsx_name + "` has no scene EventType in this slice");
        }
        const std::vector<ir::Param>* params = nullptr;
        std::string method;
        if (starts_with(target, "handler::")) {
            const ir::Handler* handler = find_handler(component, target);
            if (handler == nullptr) {
                return fail(error,
                            "event `" + jsx_name + "` names unknown handler `" + target + "`");
            }
            params = &handler->params;
            method = method_name(handler->id);
        } else if (starts_with(target, "bind::")) {
            const ir::Binding* binding = find_binding(bindings, target);
            if (binding == nullptr) {
                return fail(error,
                            "event `" + jsx_name + "` names unknown binding `" + target + "`");
            }
            params = &binding->params;
            method = binding->region;
        } else {
            return fail(error, "event `" + jsx_name + "` names neither a handler nor a binding");
        }
        bool typed = false;
        std::string param_name;
        const std::optional<std::string> signature = payload_signature(*params, typed, param_name);
        if (!signature) {
            return fail(error, "event `" + jsx_name + "` payload record `" + params->front().type +
                                   "` has no C++ record");
        }
        const std::string conversion =
            typed ? "tessera::ui::" + std::string(find_payload(params->front().type)->converter) +
                        "(event)"
                  : "event";
        EventAdapter adapter;
        adapter.ordinal = ordinal;
        adapter.jsx_name = jsx_name;
        adapter.event_type = "tessera::ui::EventType::" + std::string(mapping->enum_name);
        adapter.function = adapter_function_name(ordinal, jsx_name);
        adapter.call = "component->" + method + "(" + conversion + ");";
        adapters.push_back(std::move(adapter));
    }
    for (const ir::Node& child : node.children) {
        if (!collect_event_adapters(child, component, bindings, controlled_inputs, ordinal,
                                    adapters, error)) {
            return false;
        }
    }
    // ir-v1 section 3.5: a virtual item template is not a child, but its nodes take the same
    // pre-order ordinal as the plan and may bind events (a row click handler).
    if (node.item_template &&
        !collect_event_adapters(*node.item_template, component, bindings, controlled_inputs,
                                ordinal, adapters, error)) {
        return false;
    }
    return true;
}

void emit_event_adapters(CppWriter& writer, std::string_view component_name,
                         const std::vector<EventAdapter>& adapters) {
    if (adapters.empty()) {
        return;
    }
    writer.open("namespace");
    for (const EventAdapter& adapter : adapters) {
        writer.open("void " + adapter.function +
                    "(void* context, const tessera::ui::Event& event)");
        writer.line("auto* component = static_cast<" + std::string(component_name) +
                    "*>(context);");
        writer.line(adapter.call);
        writer.close("");
    }
    writer.close("");
    writer.line("");
}

} // namespace tessera::compiler::codegen::internal
