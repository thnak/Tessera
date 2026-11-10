// SPDX-License-Identifier: MIT
//
// Every platform without the Windows UI Automation SDK: no UIA platform seam, so `UiaHost::start`
// reports `UnsupportedPlatform` and the app keeps running with accessibility disabled (spec
// section 7). This is the only file the non-Windows build needs for the host wiring.

#include "tessera/ui/accessibility/uia_host.hpp"

namespace tessera::ui::a11y {

std::unique_ptr<UiaPlatform> create_platform_uia() {
    return nullptr;
}

} // namespace tessera::ui::a11y
