// SPDX-License-Identifier: MIT
//
// Windows-only COM fragment navigation: parent/child/sibling links, fragment root, hit-test and
// focus entry points, and the provider's behavior after the bridge stops. The vtable is called
// directly; no UIA client is involved.

#include "a11y_uia_com_test_support.hpp"
#include "uia_com.hpp"

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <iostream>
#include <windows.h>

namespace {

using namespace a11y_uia_com_test;
using a11y_uia_test::expect;
using tessera::ui::a11y::UiaBridge;

bool fragment_navigation(UiaBridge& bridge) {
    ::IRawElementProviderSimple* root = make_provider(bridge, 0);
    if (!expect(root != nullptr, "root provider created")) {
        return false;
    }
    IRawElementProviderFragment* fragment =
        query<IRawElementProviderFragment>(root, IID_IRawElementProviderFragment);
    IRawElementProviderFragmentRoot* fragment_root = nullptr;
    IRawElementProviderFragment* child = nullptr;
    if (fragment != nullptr) {
        (void)fragment->get_FragmentRoot(&fragment_root);
        (void)fragment->Navigate(NavigateDirection_FirstChild, &child);
    }
    IRawElementProviderSimple* child_simple =
        query<IRawElementProviderSimple>(child, IID_IRawElementProviderSimple);
    IRawElementProviderFragment* last = nullptr;
    if (fragment != nullptr) {
        (void)fragment->Navigate(NavigateDirection_LastChild, &last);
    }
    IRawElementProviderSimple* last_simple =
        query<IRawElementProviderSimple>(last, IID_IRawElementProviderSimple);
    IRawElementProviderFragment* parent_of_child = nullptr;
    if (child != nullptr) {
        (void)child->Navigate(NavigateDirection_Parent, &parent_of_child);
    }
    IRawElementProviderSimple* parent_simple =
        query<IRawElementProviderSimple>(parent_of_child, IID_IRawElementProviderSimple);
    IRawElementProviderFragment* next = nullptr;
    if (child != nullptr) {
        (void)child->Navigate(NavigateDirection_NextSibling, &next);
    }
    IRawElementProviderSimple* next_simple =
        query<IRawElementProviderSimple>(next, IID_IRawElementProviderSimple);

    IRawElementProviderFragment* hit = nullptr;
    if (fragment_root != nullptr) {
        (void)fragment_root->ElementProviderFromPoint(50.0, 30.0, &hit);
    }
    IRawElementProviderSimple* hit_simple =
        query<IRawElementProviderSimple>(hit, IID_IRawElementProviderSimple);
    IRawElementProviderFragment* focused = nullptr;
    if (fragment_root != nullptr) {
        (void)fragment_root->GetFocus(&focused);
    }
    IRawElementProviderSimple* focused_simple =
        query<IRawElementProviderSimple>(focused, IID_IRawElementProviderSimple);

    const bool ok =
        expect(fragment_root != nullptr, "fragment root queried") &&
        expect(child_simple != nullptr && name_is(child_simple, L"Save"),
               "first child is the button") &&
        expect(last_simple != nullptr && name_is(last_simple, L"Rows"), "last child is the list") &&
        expect(parent_simple != nullptr && name_is(parent_simple, L"Root"),
               "child navigates to parent") &&
        expect(next_simple != nullptr && name_is(next_simple, L"Email"), "sibling navigation") &&
        expect(hit_simple != nullptr && name_is(hit_simple, L"Save"), "hit test provider") &&
        expect(focused_simple != nullptr && name_is(focused_simple, L"Save"), "focus provider");

    release(focused_simple);
    release(focused);
    release(hit_simple);
    release(hit);
    release(next_simple);
    release(next);
    release(parent_simple);
    release(parent_of_child);
    release(last_simple);
    release(last);
    release(child_simple);
    release(child);
    release(fragment_root);
    release(fragment);
    release(root);
    return ok;
}

bool shutdown_answers_not_available() {
    UiaBridge bridge;
    if (!start_with_scene(bridge)) {
        return false;
    }
    ::IRawElementProviderSimple* button = make_provider(bridge, 1);
    if (!expect(button != nullptr, "provider created")) {
        return false;
    }
    bridge.stop();

    VARIANT name;
    VariantInit(&name);
    const HRESULT after_stop = button->GetPropertyValue(UIA_NamePropertyId, &name);
    VARIANT offscreen;
    VariantInit(&offscreen);
    const HRESULT static_property = button->GetPropertyValue(UIA_IsOffscreenPropertyId, &offscreen);
    IUnknown* pattern = nullptr;
    const HRESULT pattern_after_stop = button->GetPatternProvider(UIA_InvokePatternId, &pattern);
    const bool ok = expect(after_stop == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
                           "a stopped bridge answers element-not-available") &&
                    expect(SUCCEEDED(static_property) && offscreen.vt == VT_BOOL,
                           "snapshot-free properties still answer") &&
                    expect(pattern_after_stop == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) &&
                               pattern == nullptr,
                           "patterns fail after shutdown");
    VariantClear(&name);
    VariantClear(&offscreen);
    release(pattern);
    release(button);
    return ok;
}

} // namespace

int main() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        std::cerr << "a11y uia: CoInitializeEx failed\n";
        return 1;
    }
    bool navigation = false;
    {
        UiaBridge bridge;
        if (!start_with_scene(bridge)) {
            std::cerr << "a11y uia: Windows MTA bridge did not start\n";
            return 1;
        }
        navigation = fragment_navigation(bridge);
        bridge.stop();
    }
    const bool shutdown = shutdown_answers_not_available();
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (!navigation) {
        return 2;
    }
    if (!shutdown) {
        return 3;
    }
    std::cout << "a11y_uia_com_nav: ok\n";
    return 0;
}
