// SPDX-License-Identifier: MIT
//
// Windows-only: the real HAL window + real UIA platform seam answer the root's
// `get_HostRawElementProvider` with a `UiaHostProviderFromHwnd` provider for that HWND, cache it
// across queries, and fail cleanly (E_FAIL, no COM call) when no handle is installed. The client
// call runs on a second thread while the window thread pumps, so a host provider that needs a
// window message cannot deadlock the test. No Narrator/NVDA or UIA client is involved.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "a11y_uia_host_fixture.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/accessibility/uia_host.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <windows.h>

// windows.h must precede the generated UI Automation headers: the `interface` macro they rely on
// comes from basetyps.h through the non-lean windows.h include chain (rpc.h pulls it in).
#include <UIAutomationClient.h>
#include <UIAutomationCore.h>

namespace {

using tessera::hal::AccessibilityHostHandle;
using tessera::hal::AccessibilityRoot;
using tessera::ui::a11y::kUiaRootObjectId;
using tessera::ui::a11y::make_window_root_provider;
using tessera::ui::a11y::UiaHost;
using tessera::ui::a11y::UiaHostElementState;
using tessera::ui::a11y::UiaHostElementStatus;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y uia window host element: " << message << "\n";
    }
    return condition;
}

using tessera::ui::a11y::UiaHostConfig;

UiaHostConfig test_config() {
    UiaHostConfig config;
    config.bridge.idle_wait_ms = 1000;
    config.bridge.shutdown_wait_ms = 5000;
    return config;
}

struct ClientOutcome {
    HRESULT first = E_FAIL;
    HRESULT second = E_FAIL;
    ::IRawElementProviderSimple* first_provider = nullptr;
    ::IRawElementProviderSimple* second_provider = nullptr;
    bool same_provider = false;
    bool native_handle_read = false;
    LONG native_handle = 0;
};

// Runs on a non-window thread: the root marshals the host-element build to the bridge thread. The
// window thread keeps pumping so a message-based host provider can be created.
void query_host_element(::IRawElementProviderSimple* root, ClientOutcome& outcome) {
    outcome.first = root->get_HostRawElementProvider(&outcome.first_provider);
    if (outcome.first_provider != nullptr) {
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(outcome.first_provider->GetPropertyValue(UIA_NativeWindowHandlePropertyId,
                                                               &value)) &&
            value.vt == VT_I4) {
            outcome.native_handle_read = true;
            outcome.native_handle = value.lVal;
        }
        VariantClear(&value);
    }
    outcome.second = root->get_HostRawElementProvider(&outcome.second_provider);
    outcome.same_provider =
        outcome.second_provider != nullptr && outcome.second_provider == outcome.first_provider;
    if (outcome.second_provider != nullptr) {
        outcome.second_provider->Release();
    }
    if (outcome.first_provider != nullptr) {
        outcome.first_provider->Release();
    }
}

} // namespace

int main() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        std::cerr << "a11y uia window host element: CoInitializeEx failed\n";
        return 1;
    }

    const std::string title = "Tessera UIA host element " + std::to_string(GetCurrentProcessId());
    tessera::hal::WindowDesc desc;
    desc.title = title;
    desc.width = 240;
    desc.height = 160;
    desc.visible = false;
    auto window = tessera::hal::create_window(desc);
    if (!expect(window != nullptr, "create_window returns a window")) {
        return 2;
    }

    const AccessibilityHostHandle handle = window->accessibility_host_handle();
    const HWND hwnd = static_cast<HWND>(handle.token);
    if (!expect(hwnd != nullptr, "the window exposes a native host handle") ||
        !expect(window->native_handle().window == handle.token,
                "the host handle is the native window token")) {
        return 3;
    }

    UiaHost host(test_config());
    // Install twice: the platform stores the same token and the host forwards it unchanged.
    host.set_host_handle(handle);
    host.set_host_handle(handle);
    // Reinstall the WM_GETOBJECT answer: same callback/context, so the window is unaffected.
    window->set_accessibility_root(make_window_root_provider(host));
    window->set_accessibility_root(make_window_root_provider(host));

    if (!expect(host.start().has_value(), "real UIA platform starts") ||
        !expect(host.bridge().step().has_value(), "step is a barrier behind the start hook") ||
        !expect(host.ready(), "the root is published")) {
        return 4;
    }

    void* raw = nullptr;
    if (!expect(host.answer_get_object(kUiaRootObjectId, raw) && raw != nullptr,
                "the root answers WM_GETOBJECT")) {
        return 5;
    }
    auto* root = static_cast<::IRawElementProviderSimple*>(raw);
    root->AddRef();

    const AccessibilityRoot installed = make_window_root_provider(host);
    void* answered = nullptr;
    if (!expect(installed.answer != nullptr && installed.context == &host,
                "the install helper forwards to the host") ||
        !expect(installed.answer(installed.context, kUiaRootObjectId, answered) && answered == raw,
                "a reinstalled answer returns the same root")) {
        root->Release();
        return 6;
    }

    const UiaHostElementStatus before = host.host_element_status();
    if (!expect(before.handle_installed, "the handle is installed") ||
        !expect(before.state == UiaHostElementState::Pending,
                "the host provider is built only on demand")) {
        root->Release();
        return 7;
    }

    ClientOutcome outcome;
    std::atomic<bool> done{false};
    std::thread client([&] {
        query_host_element(root, outcome);
        done.store(true, std::memory_order_release);
    });
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (!done.load(std::memory_order_acquire) && GetTickCount64() < deadline) {
        (void)window->wait_for_events(0.01);
    }
    client.join();

    const UiaHostElementStatus after = host.host_element_status();
    const bool connected =
        expect(done.load(std::memory_order_acquire), "the client query completes") &&
        expect(SUCCEEDED(outcome.first) && outcome.first_provider != nullptr,
               "the root answers with a host provider") &&
        expect(SUCCEEDED(outcome.second) && outcome.same_provider,
               "the host provider is cached across queries") &&
        expect(outcome.native_handle_read &&
                   outcome.native_handle == static_cast<LONG>(reinterpret_cast<LONG_PTR>(hwnd)),
               "the host provider belongs to the window handle") &&
        expect(after.state == UiaHostElementState::Ready, "the wiring reports Ready") &&
        expect(after.failures == 0, "no failed host-element builds");

    // Without a handle the root fails that one query cleanly and does not attempt COM.
    UiaHost bare(test_config());
    if (!expect(bare.start().has_value(), "the second host starts") ||
        !expect(bare.bridge().step().has_value(), "the second host steps")) {
        root->Release();
        return 8;
    }
    void* bare_raw = nullptr;
    if (!expect(bare.answer_get_object(kUiaRootObjectId, bare_raw) && bare_raw != nullptr,
                "the handle-less root answers WM_GETOBJECT")) {
        root->Release();
        return 9;
    }
    auto* bare_root = static_cast<::IRawElementProviderSimple*>(bare_raw);
    bare_root->AddRef();
    ::IRawElementProviderSimple* bare_host = nullptr;
    const HRESULT bare_result = bare_root->get_HostRawElementProvider(&bare_host);
    const UiaHostElementStatus bare_status = bare.host_element_status();
    const bool unavailable =
        expect(bare_result == E_FAIL && bare_host == nullptr, "no handle is a clean E_FAIL") &&
        expect(!bare_status.handle_installed &&
                   bare_status.state == UiaHostElementState::Unavailable,
               "the status reports no host element") &&
        expect(bare_status.failures == 0, "no handle means no failed COM attempt");
    bare_root->Release();
    bare.stop();

    window->set_accessibility_root({});
    root->Release();
    host.stop();
    window.reset();

    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (!connected) {
        return 10;
    }
    if (!unavailable) {
        return 11;
    }
    std::cout << "a11y_uia_window_host_element: ok\n";
    return 0;
}
