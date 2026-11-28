// SPDX-License-Identifier: MIT

// The Icon gallery page (issue #530, ADR-047 item 10): every icon of the emitted set in both sizes,
// inheriting the text color of its row, and the directional chevron with and without `mirror` so the
// `--rtl` golden shows the flip on one icon only. The host switches theme and direction; the page
// holds no appearance logic.

import { Icon } from "./Icon";

export function IconsPage() {
  return (
    <View className="g-page">
      <Text className="g-title">Icon</Text>
      <View className="g-row">
        <Icon name="tabler:home" size="md" />
        <Icon name="tabler:house" size="md" />
        <Icon name="tabler:check" size="md" />
        <Icon name="tabler:trash" size="md" />
        <Icon name="tabler:chevron-right" size="md" />
        <Icon name="mdi:account" size="md" />
      </View>
      <View className="g-row">
        <Icon name="tabler:home" size="sm" />
        <Icon name="tabler:house" size="sm" />
        <Icon name="tabler:check" size="sm" />
        <Icon name="tabler:trash" size="sm" />
        <Icon name="tabler:chevron-right" size="sm" />
        <Icon name="mdi:account" size="sm" />
      </View>
      <View className="g-row ip-accent">
        <Icon name="tabler:home" size="md" />
        <Icon name="tabler:house" size="md" />
        <Icon name="tabler:check" size="md" />
        <Icon name="tabler:trash" size="md" />
        <Icon name="tabler:chevron-right" size="md" />
        <Icon name="mdi:account" size="md" />
      </View>
      <View className="g-row">
        <Icon name="tabler:chevron-right" label="Next" mirror={true} />
        <Icon name="tabler:chevron-right" label="Next, not mirrored" />
        <Icon name="tabler:chevron-right" size="sm" mirror={true} />
        <Icon name="tabler:chevron-right" size="sm" />
      </View>
    </View>
  );
}
