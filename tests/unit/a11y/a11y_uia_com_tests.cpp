// SPDX-License-Identifier: MIT
//
// Windows-only COM provider properties and patterns: Name/ControlType/IsEnabled/BoundingRectangle/
// Value, provider options, runtime id, Invoke/Value/SelectionItem, and the queued UI-thread
// actions. The vtable is called directly; no UIA client is involved.

#include "a11y_uia_com_test_support.hpp"
#include "uia_com.hpp"

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <iostream>
#include <vector>
#include <windows.h>

namespace {

using namespace a11y_uia_com_test;
using a11y_uia_test::expect;
using a11y_uia_test::expect_eq;
using a11y_uia_test::expect_equal;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::text_of;
using tessera::ui::a11y::UiaActionRequest;
using tessera::ui::a11y::UiaBridge;

bool properties(UiaBridge& bridge) {
    ::IRawElementProviderSimple* root = make_provider(bridge, 0);
    ::IRawElementProviderSimple* button = make_provider(bridge, 1);
    if (!expect(root != nullptr && button != nullptr, "providers created on the bridge thread")) {
        release(root);
        release(button);
        return false;
    }

    ProviderOptions options = ProviderOptions_ClientSideProvider;
    VARIANT name;
    VariantInit(&name);
    VARIANT control;
    VariantInit(&control);
    VARIANT enabled;
    VariantInit(&enabled);
    VARIANT rect;
    VariantInit(&rect);
    VARIANT offscreen;
    VariantInit(&offscreen);
    VARIANT framework;
    VariantInit(&framework);
    VARIANT automation;
    VariantInit(&automation);
    VARIANT unknown;
    VariantInit(&unknown);

    const HRESULT options_result = button->get_ProviderOptions(&options);
    const HRESULT name_result = button->GetPropertyValue(UIA_NamePropertyId, &name);
    const HRESULT control_result = button->GetPropertyValue(UIA_ControlTypePropertyId, &control);
    const HRESULT enabled_result = button->GetPropertyValue(UIA_IsEnabledPropertyId, &enabled);
    const HRESULT rect_result = button->GetPropertyValue(UIA_BoundingRectanglePropertyId, &rect);
    const HRESULT offscreen_result =
        button->GetPropertyValue(UIA_IsOffscreenPropertyId, &offscreen);
    const HRESULT framework_result =
        button->GetPropertyValue(UIA_FrameworkIdPropertyId, &framework);
    const HRESULT automation_result =
        button->GetPropertyValue(UIA_AutomationIdPropertyId, &automation);
    const HRESULT unknown_result = button->GetPropertyValue(424242, &unknown);

    double rectangle[4] = {};
    bool rect_ok = SUCCEEDED(rect_result) && rect.vt == (VT_ARRAY | VT_R8);
    for (LONG index = 0; rect_ok && index < 4; ++index) {
        double value = 0.0;
        rect_ok = SafeArrayGetElement(rect.parray, &index, &value) == S_OK;
        rectangle[index] = value;
    }

    ::IRawElementProviderSimple* host = nullptr;
    const HRESULT host_result = button->get_HostRawElementProvider(&host);

    IRawElementProviderFragment* fragment =
        query<IRawElementProviderFragment>(button, IID_IRawElementProviderFragment);
    SAFEARRAY* runtime = nullptr;
    const HRESULT runtime_result =
        fragment != nullptr ? fragment->GetRuntimeId(&runtime) : E_NOINTERFACE;
    LONG runtime_kind = 0;
    LONG runtime_index = 0;
    LONG slot0 = 0;
    LONG slot1 = 1;
    const bool runtime_ok = SUCCEEDED(runtime_result) && runtime != nullptr &&
                            SafeArrayGetElement(runtime, &slot0, &runtime_kind) == S_OK &&
                            SafeArrayGetElement(runtime, &slot1, &runtime_index) == S_OK &&
                            runtime_kind == UiaAppendRuntimeId && runtime_index == 2;

    const bool ok =
        expect(SUCCEEDED(options_result) && (options & ProviderOptions_UseComThreading) != 0 &&
                   (options & ProviderOptions_ServerSideProvider) != 0,
               "provider options are server-side and COM-threaded") &&
        expect(SUCCEEDED(name_result) && name.vt == VT_BSTR &&
                   std::wstring(name.bstrVal) == L"Save",
               "Name property") &&
        expect(SUCCEEDED(control_result) && control.vt == VT_I4 &&
                   control.lVal == UIA_ButtonControlTypeId,
               "ControlType property") &&
        expect(SUCCEEDED(enabled_result) && enabled.vt == VT_BOOL &&
                   enabled.boolVal == VARIANT_TRUE,
               "IsEnabled property") &&
        expect(rect_ok && rectangle[0] == 10.0 && rectangle[1] == 20.0 && rectangle[2] == 100.0 &&
                   rectangle[3] == 32.0,
               "BoundingRectangle property") &&
        expect(SUCCEEDED(offscreen_result) && offscreen.vt == VT_BOOL &&
                   offscreen.boolVal == VARIANT_FALSE,
               "IsOffscreen property") &&
        expect(SUCCEEDED(framework_result) && framework.vt == VT_BSTR &&
                   std::wstring(framework.bstrVal) == L"Tessera",
               "FrameworkId property") &&
        expect(SUCCEEDED(automation_result) && automation.vt == VT_BSTR &&
                   std::wstring(automation.bstrVal) == L"7",
               "AutomationId property") &&
        expect(SUCCEEDED(unknown_result) && unknown.vt == VT_EMPTY,
               "unknown property is VT_EMPTY") &&
        expect(SUCCEEDED(host_result) && host == nullptr, "host provider is null") &&
        expect(runtime_ok, "runtime id") && expect(name_is(root, L"Root"), "root name");

    if (runtime != nullptr) {
        SafeArrayDestroy(runtime);
    }
    VariantClear(&name);
    VariantClear(&control);
    VariantClear(&enabled);
    VariantClear(&rect);
    VariantClear(&offscreen);
    VariantClear(&framework);
    VariantClear(&automation);
    VariantClear(&unknown);
    release(fragment);
    release(root);
    release(button);
    return ok;
}

bool patterns_and_actions(UiaBridge& bridge) {
    ::IRawElementProviderSimple* button = make_provider(bridge, 1);
    ::IRawElementProviderSimple* field = make_provider(bridge, 2);
    ::IRawElementProviderSimple* item = make_provider(bridge, 4);
    if (!expect(button != nullptr && field != nullptr && item != nullptr,
                "pattern providers created")) {
        release(button);
        release(field);
        release(item);
        return false;
    }

    IUnknown* invoke_unknown = nullptr;
    IUnknown* button_value_unknown = nullptr;
    IUnknown* field_value_unknown = nullptr;
    IUnknown* selection_unknown = nullptr;
    const HRESULT invoke_result = button->GetPatternProvider(UIA_InvokePatternId, &invoke_unknown);
    const HRESULT button_value_result =
        button->GetPatternProvider(UIA_ValuePatternId, &button_value_unknown);
    const HRESULT field_value_result =
        field->GetPatternProvider(UIA_ValuePatternId, &field_value_unknown);
    const HRESULT selection_result =
        item->GetPatternProvider(UIA_SelectionItemPatternId, &selection_unknown);

    IInvokeProvider* invoke = query<IInvokeProvider>(invoke_unknown, IID_IInvokeProvider);
    IValueProvider* field_value = query<IValueProvider>(field_value_unknown, IID_IValueProvider);
    ISelectionItemProvider* selection =
        query<ISelectionItemProvider>(selection_unknown, IID_ISelectionItemProvider);
    IRawElementProviderFragment* fragment =
        query<IRawElementProviderFragment>(button, IID_IRawElementProviderFragment);

    HRESULT invoke_call = E_FAIL;
    HRESULT focus_call = E_FAIL;
    HRESULT set_value_call = E_FAIL;
    BOOL is_selected = FALSE;
    BOOL is_read_only = TRUE;
    BSTR field_text = nullptr;
    if (invoke != nullptr) {
        invoke_call = invoke->Invoke();
    }
    if (fragment != nullptr) {
        focus_call = fragment->SetFocus();
    }
    if (field_value != nullptr) {
        set_value_call = field_value->SetValue(L"new value");
        (void)field_value->get_Value(&field_text);
        (void)field_value->get_IsReadOnly(&is_read_only);
    }
    if (selection != nullptr) {
        (void)selection->get_IsSelected(&is_selected);
    }

    std::vector<UiaActionRequest> actions(8);
    const std::size_t taken = bridge.take_actions(actions);
    const bool ok =
        expect(SUCCEEDED(invoke_result) && invoke != nullptr, "invoke pattern advertised") &&
        expect(SUCCEEDED(button_value_result) && button_value_unknown == nullptr,
               "button does not advertise value") &&
        expect(SUCCEEDED(field_value_result) && field_value != nullptr, "field advertises value") &&
        expect(SUCCEEDED(selection_result) && selection != nullptr,
               "selection item pattern advertised") &&
        expect(SUCCEEDED(invoke_call), "Invoke accepted") &&
        expect(SUCCEEDED(focus_call), "SetFocus accepted") &&
        expect(SUCCEEDED(set_value_call), "SetValue accepted") &&
        expect(is_selected == TRUE, "IsSelected") &&
        expect(is_read_only == FALSE, "field is editable") &&
        expect(field_text != nullptr && std::wstring(field_text) == L"hello", "Value property") &&
        expect_eq(static_cast<std::int64_t>(taken), 3, "three UI-thread actions") &&
        expect(actions[0].action == Action::Press, "invoke queued a press") &&
        expect(actions[1].action == Action::Focus, "focus queued") &&
        expect(actions[2].action == Action::SetValue, "value queued") &&
        expect_equal(text_of(actions[2]), "new value", "SetValue text");

    if (field_text != nullptr) {
        SysFreeString(field_text);
    }
    release(invoke);
    release(field_value);
    release(selection);
    release(fragment);
    release(invoke_unknown);
    release(button_value_unknown);
    release(field_value_unknown);
    release(selection_unknown);
    release(button);
    release(field);
    release(item);
    return ok;
}

} // namespace

int main() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        std::cerr << "a11y uia: CoInitializeEx failed\n";
        return 1;
    }
    UiaBridge bridge;
    if (!start_with_scene(bridge)) {
        std::cerr << "a11y uia: Windows MTA bridge did not start\n";
        return 1;
    }
    const bool props = properties(bridge);
    const bool patterns = patterns_and_actions(bridge);
    bridge.stop();
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (!props) {
        return 2;
    }
    if (!patterns) {
        return 3;
    }
    std::cout << "a11y_uia_com: ok\n";
    return 0;
}
