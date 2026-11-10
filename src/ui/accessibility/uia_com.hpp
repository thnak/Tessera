// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/uia_bridge.hpp"

#if !defined(_WIN32)
#error "uia_com.hpp is Windows-only; guard the include with #if defined(_WIN32)"
#endif

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>

namespace tessera::ui::a11y::win {

// Creates one COM provider object for snapshot `index`, with a reference count of one. The object
// implements IRawElementProviderSimple, IRawElementProviderFragment (+Root for index 0),
// IInvokeProvider, IValueProvider, and ISelectionItemProvider, and marshals every call back onto
// the bridge thread (`UiaBridge::run_on_bridge`); it never touches the scene.
//
// The host owns the returned reference. Release every provider before destroying the bridge: after
// `stop()` the object answers `UIA_E_ELEMENTNOTAVAILABLE` instead of reading stale state. This
// header is internal to the Windows translation units and must only be included under `_WIN32`.
[[nodiscard]] ::IRawElementProviderSimple* create_uia_provider(UiaBridge& bridge, A11yIndex index);

} // namespace tessera::ui::a11y::win
