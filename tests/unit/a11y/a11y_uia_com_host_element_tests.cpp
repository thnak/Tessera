// SPDX-License-Identifier: MIT
//
// Windows-only: the root provider's `get_HostRawElementProvider`. A fake HostElementSource stands
// in for `UiaHostProviderFromHwnd`, so the test pins the COM contract deterministically and never
// needs a screen reader: an AddRef'd provider when connected, E_FAIL when the source is missing,
// unavailable, failed, or the bridge stopped, and S_OK + null for a child fragment.

#include "a11y_uia_com_test_support.hpp"
#include "uia_com.hpp"

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <windows.h>

namespace {

using tessera::ui::a11y::A11yIndex;
using tessera::ui::a11y::UiaBridge;
using tessera::ui::a11y::UiaProvider;
using tessera::ui::a11y::win::HostElement;
using tessera::ui::a11y::win::HostElementSource;
using tessera::ui::a11y::win::HostElementState;

// Minimal host element: identity, no patterns, no properties. It only has to prove ownership and
// pointer identity across repeated root queries.
class FakeHostProvider final : public ::IRawElementProviderSimple {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) {
            return E_INVALIDARG;
        }
        *out = nullptr;
        if (InlineIsEqualGUID(riid, IID_IUnknown) ||
            InlineIsEqualGUID(riid, IID_IRawElementProviderSimple)) {
            *out = static_cast<::IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(refs_.fetch_add(1) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = refs_.fetch_sub(1) - 1;
        if (remaining == 0) {
            delete this;
        }
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        if (out == nullptr) {
            return E_INVALIDARG;
        }
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** out) override {
        if (out == nullptr) {
            return E_INVALIDARG;
        }
        *out = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID, VARIANT* out) override {
        if (out == nullptr) {
            return E_INVALIDARG;
        }
        VariantInit(out);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    get_HostRawElementProvider(::IRawElementProviderSimple** out) override {
        if (out == nullptr) {
            return E_INVALIDARG;
        }
        *out = nullptr;
        return S_OK;
    }

    [[nodiscard]] LONG refs() const {
        return refs_.load();
    }

private:
    std::atomic<LONG> refs_{1};
};

class FakeHostSource final : public HostElementSource {
public:
    [[nodiscard]] HostElement acquire() override {
        ++acquires;
        if (state == HostElementState::Ready && provider != nullptr) {
            provider->AddRef();
            return {provider, HostElementState::Ready};
        }
        if (state == HostElementState::Failed) {
            return {nullptr, HostElementState::Failed};
        }
        return {};
    }

    HostElementState state = HostElementState::Unavailable;
    ::IRawElementProviderSimple* provider = nullptr;
    int acquires = 0;
};

::IRawElementProviderSimple* make_root(UiaBridge& bridge, HostElementSource* source,
                                       A11yIndex index = 0) {
    ::IRawElementProviderSimple* provider = nullptr;
    const auto created = bridge.run_on_bridge([&](UiaProvider&) {
        provider = tessera::ui::a11y::win::create_uia_provider(bridge, index, source);
    });
    return created.has_value() ? provider : nullptr;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y uia host element: " << message << "\n";
    }
    return condition;
}

// Connected: the root returns the source provider, AddRef'd, and caches it across queries.
bool connected_source_answers_with_a_reference() {
    UiaBridge bridge;
    if (!a11y_uia_com_test::start_with_scene(bridge)) {
        return expect(false, "bridge starts");
    }
    auto* host = new FakeHostProvider();
    FakeHostSource source;
    source.state = HostElementState::Ready;
    source.provider = host;
    ::IRawElementProviderSimple* root = make_root(bridge, &source);
    bool ok = expect(root != nullptr, "root provider created");

    ::IRawElementProviderSimple* first = nullptr;
    ::IRawElementProviderSimple* second = nullptr;
    const HRESULT first_result =
        ok ? root->get_HostRawElementProvider(&first) : static_cast<HRESULT>(E_FAIL);
    const HRESULT second_result =
        ok ? root->get_HostRawElementProvider(&second) : static_cast<HRESULT>(E_FAIL);
    ok = expect(SUCCEEDED(first_result) && first == host, "the host provider is returned") && ok;
    ok = expect(SUCCEEDED(second_result) && second == first,
                "a repeated query returns the same provider") &&
         ok;
    ok = expect(host->refs() == 3, "each answer carries its own reference") && ok;
    ok = expect(source.acquires == 2, "the source is asked once per query") && ok;

    a11y_uia_com_test::release(second);
    a11y_uia_com_test::release(first);
    a11y_uia_com_test::release(host);
    a11y_uia_com_test::release(root);
    bridge.stop();
    return ok;
}

// A child fragment is not a host element: S_OK + null, and the source is never touched.
bool child_fragment_has_no_host_provider() {
    UiaBridge bridge;
    if (!a11y_uia_com_test::start_with_scene(bridge)) {
        return expect(false, "bridge starts for a child");
    }
    FakeHostSource source;
    source.state = HostElementState::Ready;
    ::IRawElementProviderSimple* child = make_root(bridge, &source, 1);
    ::IRawElementProviderSimple* host = nullptr;
    const HRESULT result =
        child != nullptr ? child->get_HostRawElementProvider(&host) : static_cast<HRESULT>(E_FAIL);
    const bool ok = expect(SUCCEEDED(result) && host == nullptr, "a child keeps S_OK + null") &&
                    expect(source.acquires == 0, "a child never asks the platform source");
    a11y_uia_com_test::release(child);
    bridge.stop();
    return ok;
}

// No source, no handle, or a latched failure: E_FAIL + null, never a providerless element.
bool unavailable_paths_fail_cleanly() {
    UiaBridge bridge;
    if (!a11y_uia_com_test::start_with_scene(bridge)) {
        return expect(false, "bridge starts for the failure paths");
    }
    FakeHostSource source;
    ::IRawElementProviderSimple* without_source = make_root(bridge, nullptr);
    ::IRawElementProviderSimple* unavailable = make_root(bridge, &source);

    ::IRawElementProviderSimple* no_source_host = nullptr;
    ::IRawElementProviderSimple* unavailable_host = nullptr;
    const HRESULT no_source_result =
        without_source != nullptr ? without_source->get_HostRawElementProvider(&no_source_host)
                                  : static_cast<HRESULT>(S_OK);
    const HRESULT unavailable_result =
        unavailable != nullptr ? unavailable->get_HostRawElementProvider(&unavailable_host)
                               : static_cast<HRESULT>(S_OK);

    source.state = HostElementState::Failed;
    ::IRawElementProviderSimple* failed = make_root(bridge, &source);
    ::IRawElementProviderSimple* failed_host = nullptr;
    const HRESULT failed_result = failed != nullptr
                                      ? failed->get_HostRawElementProvider(&failed_host)
                                      : static_cast<HRESULT>(S_OK);

    const bool ok =
        expect(no_source_result == E_FAIL && no_source_host == nullptr,
               "no platform source is E_FAIL") &&
        expect(unavailable_result == E_FAIL && unavailable_host == nullptr,
               "no installed handle is E_FAIL") &&
        expect(failed_result == E_FAIL && failed_host == nullptr, "a failed build is E_FAIL");
    a11y_uia_com_test::release(without_source);
    a11y_uia_com_test::release(unavailable);
    a11y_uia_com_test::release(failed);
    bridge.stop();
    return ok;
}

// A stopped bridge is "not connected": E_FAIL without touching the platform source.
bool stopped_bridge_fails_without_a_com_call() {
    UiaBridge bridge;
    if (!a11y_uia_com_test::start_with_scene(bridge)) {
        return expect(false, "bridge starts before stop");
    }
    FakeHostSource source;
    source.state = HostElementState::Ready;
    ::IRawElementProviderSimple* root = make_root(bridge, &source);
    bridge.stop();
    ::IRawElementProviderSimple* host = nullptr;
    const HRESULT result =
        root != nullptr ? root->get_HostRawElementProvider(&host) : static_cast<HRESULT>(S_OK);
    const bool ok =
        expect(result == E_FAIL && host == nullptr, "a stopped bridge answers E_FAIL") &&
        expect(source.acquires == 0, "a stopped bridge calls no source");
    a11y_uia_com_test::release(root);
    return ok;
}

} // namespace

int main() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        std::cerr << "a11y uia host element: CoInitializeEx failed\n";
        return 1;
    }
    const bool connected = connected_source_answers_with_a_reference();
    const bool child = child_fragment_has_no_host_provider();
    const bool unavailable = unavailable_paths_fail_cleanly();
    const bool stopped = stopped_bridge_fails_without_a_com_call();
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (!connected) {
        return 2;
    }
    if (!child) {
        return 3;
    }
    if (!unavailable) {
        return 4;
    }
    if (!stopped) {
        return 5;
    }
    std::cout << "a11y_uia_com_host_element: ok\n";
    return 0;
}
