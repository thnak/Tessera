// SPDX-License-Identifier: MIT

// The token swatch page (issue #280, ADR-047 item 5): every colour token of the Instrument theme as
// a filled chip with its name, so a golden per theme shows the whole palette at once. The host
// switches the theme (`--theme`); the page holds no appearance logic. Names are painted with the
// bundled font pack's Plex Sans.

export function SwatchPage() {
  return (
    <View className="sw-page">
      <Text className="sw-title">Instrument tokens</Text>
      <View className="sw-grid">
        <View className="sw-cell">
          <View className="sw-chip sw-surface"></View>
          <Text className="sw-name">surface</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-surface-raised"></View>
          <Text className="sw-name">surface-raised</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-surface-sunken"></View>
          <Text className="sw-name">surface-sunken</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-on-surface"></View>
          <Text className="sw-name">on-surface</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-on-surface-muted"></View>
          <Text className="sw-name">on-surface-muted</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-border"></View>
          <Text className="sw-name">border</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-border-strong"></View>
          <Text className="sw-name">border-strong</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-accent"></View>
          <Text className="sw-name">accent</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-on-accent"></View>
          <Text className="sw-name">on-accent</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-danger"></View>
          <Text className="sw-name">danger</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-on-danger"></View>
          <Text className="sw-name">on-danger</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-success"></View>
          <Text className="sw-name">success</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-on-success"></View>
          <Text className="sw-name">on-success</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-warning"></View>
          <Text className="sw-name">warning</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-on-warning"></View>
          <Text className="sw-name">on-warning</Text>
        </View>
        <View className="sw-cell">
          <View className="sw-chip sw-focus-ring"></View>
          <Text className="sw-name">focus-ring</Text>
        </View>
      </View>
    </View>
  );
}
