// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/uia_bridge.hpp"

#if !defined(_WIN32)
#error "uia_com.hpp is Windows-only; guard the include with #if defined(_WIN32)"
#endif

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <cstdint>

namespace tessera::ui::a11y::win {

enum class HostElementState : std::uint8_t {
    Unavailable, // no native handle is installed
    Ready,       // `provider` is an AddRef'd host element provider
    Failed, // the handle produced no provider; the source latches this until the handle changes
};

// One answer to the root's host-element request. `provider` is AddRef'd for the caller and null
// unless `state == Ready`.
struct HostElement {
    ::IRawElementProviderSimple* provider = nullptr;
    HostElementState state = HostElementState::Unavailable;
};

// Platform-side supplier of the window's UIA host element (`UiaHostProviderFromHwnd` on Windows).
// The root provider only calls it from the bridge thread, so an implementation may create and cache
// the host provider there; it must never call COM on the window thread and must not block.
class HostElementSource {
public:
    virtual ~HostElementSource() = default;

    [[nodiscard]] virtual HostElement acquire() = 0;
};

// Creates one COM provider object for snapshot `index`, with a reference count of one. The object
// implements IRawElementProviderSimple, IRawElementProviderFragment (+Root for index 0),
// IInvokeProvider, IValueProvider, and ISelectionItemProvider, and marshals every call back onto
// the bridge thread (`UiaBridge::run_on_bridge`); it never touches the scene.
//
// `host_element` supplies the native host element for the fragment root (index 0). A null source,
// a missing handle, or a failed build makes `get_HostRawElementProvider` return E_FAIL instead of
// a providerless element.
//
// The host owns the returned reference. Release every provider before destroying the bridge: after
// `stop()` the object answers `UIA_E_ELEMENTNOTAVAILABLE` instead of reading stale state. This
// header is internal to the Windows translation units and must only be included under `_WIN32`.
[[nodiscard]] ::IRawElementProviderSimple*
create_uia_provider(UiaBridge& bridge, A11yIndex index, HostElementSource* host_element = nullptr);

} // namespace tessera::ui::a11y::win
