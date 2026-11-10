// SPDX-License-Identifier: MIT
//
// Shared helpers for the Windows-only COM provider tests: a deterministic scene, provider creation
// on the bridge thread, interface queries, and a name assertion. No test talks to a UIA client.

#pragma once

#include "a11y_uia_test_support.hpp"
#include "tessera/ui/accessibility/uia_bridge.hpp"
#include "tessera/ui/accessibility/uia_mapping.hpp"
#include "uia_com.hpp"

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <string>
#include <windows.h>

namespace a11y_uia_com_test {

using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::State;
using tessera::ui::a11y::UiaBridge;
using tessera::ui::a11y::UiaProvider;

inline A11yNode box(Role role, std::string_view text, float x, float y, float width, float height) {
    A11yNode node = a11y_uia_test::make_node(role, text);
    node.bounds.x = x;
    node.bounds.y = y;
    node.bounds.width = width;
    node.bounds.height = height;
    return node;
}

// Root pane, a focused button, an editable text field, a list, and a selected list item.
inline A11ySnapshot scene() {
    A11yNode root = box(Role::Generic, "Root", 0.0F, 0.0F, 800.0F, 600.0F);
    A11yNode button = box(Role::Button, "Save", 10.0F, 20.0F, 100.0F, 32.0F);
    button.actions = Action::Press | Action::Focus;
    button.states = State::Focusable | State::Focused;
    button.automation_id = 7;
    A11yNode field = box(Role::TextField, "Email", 10.0F, 60.0F, 200.0F, 28.0F);
    field.actions = Action::SetValue | Action::Focus;
    a11y_uia_test::set_text(field.value, "hello");
    A11yNode list = box(Role::List, "Rows", 0.0F, 100.0F, 800.0F, 200.0F);
    A11yNode item = box(Role::ListItem, "Row 1", 0.0F, 100.0F, 800.0F, 40.0F);
    item.actions = Action::Press;
    item.states = State::Selected;
    a11y_uia_test::link_children(root, {1, 2, 3});
    a11y_uia_test::link_children(list, {4});
    return a11y_uia_test::snapshot_of({root, button, field, list, item});
}

template<typename T> void release(T*& pointer) {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

template<typename T> T* query(IUnknown* object, REFIID iid) {
    if (object == nullptr) {
        return nullptr;
    }
    T* result = nullptr;
    if (SUCCEEDED(object->QueryInterface(iid, reinterpret_cast<void**>(&result)))) {
        return result;
    }
    return nullptr;
}

inline bool name_is(::IRawElementProviderSimple* provider, const wchar_t* expected) {
    if (provider == nullptr) {
        return false;
    }
    VARIANT value;
    VariantInit(&value);
    const HRESULT result = provider->GetPropertyValue(UIA_NamePropertyId, &value);
    const bool ok = SUCCEEDED(result) && value.vt == VT_BSTR &&
                    std::wstring(value.bstrVal) == std::wstring(expected);
    VariantClear(&value);
    return ok;
}

// Creates one provider object on the bridge thread (the object's contract requires that).
inline ::IRawElementProviderSimple* make_provider(UiaBridge& bridge,
                                                  tessera::ui::a11y::A11yIndex index) {
    ::IRawElementProviderSimple* provider = nullptr;
    const auto created = bridge.run_on_bridge([&](UiaProvider&) {
        provider = tessera::ui::a11y::win::create_uia_provider(bridge, index);
    });
    if (!created.has_value()) {
        return nullptr;
    }
    return provider;
}

// Starts a bridge, publishes the scene, and adopts it. Returns false on any failure.
inline bool start_with_scene(UiaBridge& bridge) {
    if (!bridge.start().has_value() || !bridge.step().has_value() || !bridge.running()) {
        return false;
    }
    bridge.publish(scene());
    return bridge.step().has_value();
}

} // namespace a11y_uia_com_test
