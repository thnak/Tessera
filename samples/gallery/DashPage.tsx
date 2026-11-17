// SPDX-License-Identifier: MIT

// A second parity page, chosen to hit what the form does not: a gradient hero, round avatars,
// progress bars that clip their fill to a rounded track, a table with per-side borders, a tab strip
// with an underline, wrapped chips, an absolutely placed dot, a rotated square, letter-spacing and
// upper-case text, right-aligned numbers and reduced opacity. Run it in the dark theme.
// The browser twin is `tools/parity/dash.html`; keep the two structures identical.

export function DashPage() {
  return (
    <View className="d-page">
      <View className="d-top">
        <View className="d-avatar d-avatar-a"><Text className="d-avatar-text">AL</Text></View>
        <View className="d-top-text">
          <Text className="d-hello">Good evening, Ada</Text>
          <Text className="d-date">Tuesday, 17 November</Text>
        </View>
        <View className="d-bell">
          <View className="d-bell-body"></View>
          <View className="d-dot"></View>
        </View>
      </View>

      <View className="d-hero">
        <Text className="d-eyebrow">Monthly revenue</Text>
        <Text className="d-big">$48,210</Text>
        <Text className="d-delta">+12.4% from last month</Text>
        <View className="d-spark">
          <View className="d-bar d-bar-1"></View>
          <View className="d-bar d-bar-2"></View>
          <View className="d-bar d-bar-3"></View>
          <View className="d-bar d-bar-4"></View>
          <View className="d-bar d-bar-5"></View>
          <View className="d-bar d-bar-6"></View>
        </View>
      </View>

      <View className="d-tabs">
        <View className="d-tab d-tab-on"><Text className="d-tab-text d-tab-text-on">Overview</Text></View>
        <View className="d-tab"><Text className="d-tab-text">Orders</Text></View>
        <View className="d-tab"><Text className="d-tab-text">Customers</Text></View>
        <View className="d-tab d-tab-dim"><Text className="d-tab-text">Reports</Text></View>
      </View>

      <View className="d-stats">
        <View className="d-stat">
          <Text className="d-stat-label">Conversion</Text>
          <Text className="d-stat-value">3.8%</Text>
          <View className="d-track"><View className="d-fill d-fill-a"></View></View>
        </View>
        <View className="d-stat">
          <Text className="d-stat-label">Refunds</Text>
          <Text className="d-stat-value">0.9%</Text>
          <View className="d-track"><View className="d-fill d-fill-b"></View></View>
        </View>
        <View className="d-stat">
          <Text className="d-stat-label">Uptime</Text>
          <Text className="d-stat-value">99.97%</Text>
          <View className="d-track"><View className="d-fill d-fill-c"></View></View>
        </View>
      </View>

      <View className="d-table">
        <View className="d-tr d-th">
          <Text className="d-th-text d-c1">Order</Text>
          <Text className="d-th-text d-c2">Customer</Text>
          <Text className="d-th-text d-c3">Total</Text>
        </View>
        <View className="d-tr">
          <Text className="d-td d-c1">#1042</Text>
          <Text className="d-td d-c2">Grace Hopper</Text>
          <Text className="d-td d-c3">$1,240.00</Text>
        </View>
        <View className="d-tr d-tr-alt">
          <Text className="d-td d-c1">#1041</Text>
          <Text className="d-td d-c2">Alan Turing</Text>
          <Text className="d-td d-c3">$86.50</Text>
        </View>
        <View className="d-tr">
          <Text className="d-td d-c1">#1040</Text>
          <Text className="d-td d-c2">Katherine Johnson</Text>
          <Text className="d-td d-c3">$9,310.25</Text>
        </View>
      </View>

      <View className="d-chips">
        <View className="d-chip"><View className="d-chip-dot"></View><Text className="d-chip-text">Design</Text></View>
        <View className="d-chip d-chip-on"><Text className="d-chip-text d-chip-text-on">Engineering</Text></View>
        <View className="d-chip"><Text className="d-chip-text">Support</Text></View>
        <View className="d-chip"><Text className="d-chip-text">Finance</Text></View>
        <View className="d-chip d-chip-on"><Text className="d-chip-text d-chip-text-on">Operations</Text></View>
        <View className="d-chip"><Text className="d-chip-text">Legal</Text></View>
        <View className="d-chip"><View className="d-chip-turn"></View><Text className="d-chip-text">Archive</Text></View>
      </View>
    </View>
  );
}
