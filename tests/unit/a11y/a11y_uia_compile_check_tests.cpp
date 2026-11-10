// SPDX-License-Identifier: MIT
//
// Windows-only compile/link check for the UIA bridge: `UIAutomationCore.h`/`UIAutomationClient.h`
// are available, the id constants in the platform-neutral mapping match the SDK, and
// `uiautomationcore` + `ole32` link from the static accessibility library. It never creates a
// provider and never talks to a real client.

#include "uia_sdk_probe.hpp"

#include <iostream>

int main() {
    if (!tessera::ui::a11y::win::uia_sdk_probe()) {
        std::cerr << "a11y uia: UIAutomationCore/ole32 probe failed\n";
        return 1;
    }
    std::cout << "a11y_uia_compile_check: ok\n";
    return 0;
}
