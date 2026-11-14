// SPDX-License-Identifier: MIT

// Gallery page of the library SafeArea (issue #223, ADR-047 item 10): the window is tinted so the
// unsafe area (notch, bars, keyboard, hinge) shows, and the SafeArea keeps a content panel clear of
// it. The host (main.cpp) scripts the device with `--device`; the page itself knows nothing about it.

import { SafeArea } from "./SafeArea";

export function SafeAreaPage() {
  return (
    <View className="g-sa-window">
      <SafeArea sources="system-bars+cutout+ime+gestures+hinge">
        <View className="g-sa-panel">
          <Text className="g-title">SafeArea</Text>
          <Text className="g-note">Content stays inside the panel</Text>
        </View>
      </SafeArea>
    </View>
  );
}
