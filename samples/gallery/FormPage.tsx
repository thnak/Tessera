// SPDX-License-Identifier: MIT

// A deliberately busy settings form, used to compare what the engine paints with what a browser
// paints for the same markup and the same stylesheet (`form.css`). It is layout and paint stress,
// not an API sample: a card with a shadow, a header with a badge, a two-column grid, text inputs
// (one in an error state), a faux select, a segmented control, switches and a checkbox drawn from
// plain views, a truncated line, a divider and a footer of buttons in every variant and state.
// The browser twin is `tools/parity/form.html`; keep the two structures identical.

import { useState } from "tessera";
import { Button } from "./Button";

export function FormPage() {
  const [submits, setSubmits] = useState<int>(0);
  const [plan, setPlan] = useState<int>(0);
  return (
    <View className="f-page">
      <View className="f-card">
        <View className="f-head">
          <View className="f-logo">
            <Text className="f-logo-mark">T</Text>
          </View>
          <View className="f-head-text">
            <Text className="f-title">Create workspace</Text>
            <Text className="f-sub">Invite your team and pick a plan. You can change this later.</Text>
          </View>
          <View className="f-badge">
            <Text className="f-badge-text">Pro trial</Text>
          </View>
        </View>

        <View className="f-grid">
          <View className="f-field">
            <Text className="f-label">First name</Text>
            <TextInput className="f-input" aria-label="First name" defaultValue="Ada" />
          </View>
          <View className="f-field">
            <Text className="f-label">Last name</Text>
            <TextInput className="f-input" aria-label="Last name" defaultValue="Lovelace" />
          </View>
          <View className="f-field f-span">
            <Text className="f-label">Work email</Text>
            <TextInput className="f-input f-input-error" aria-label="Work email" defaultValue="ada@analytical" />
            <Text className="f-error">Enter a full address, for example ada@analytical.engine</Text>
          </View>
          <View className="f-field f-span">
            <Text className="f-label">Region</Text>
            <View className="f-select">
              <Text className="f-select-value">Europe / Berlin (UTC+01:00)</Text>
              <Text className="f-select-caret">v</Text>
            </View>
            <Text className="f-help f-ellipsis">Data stays in this region. Backups are kept in the same country unless you opt in to a second site later.</Text>
          </View>
        </View>

        <View className="f-segment">
          <View className={`f-seg ${plan === 0 ? "f-seg-on" : ""}`} onClick={() => setPlan(0)}>
            <Text className={`f-seg-text ${plan === 0 ? "f-seg-text-on" : ""}`}>Monthly</Text>
          </View>
          <View className={`f-seg ${plan === 1 ? "f-seg-on" : ""}`} onClick={() => setPlan(1)}>
            <Text className={`f-seg-text ${plan === 1 ? "f-seg-text-on" : ""}`}>Yearly</Text>
          </View>
          <View className={`f-seg ${plan === 2 ? "f-seg-on" : ""}`} onClick={() => setPlan(2)}>
            <Text className={`f-seg-text ${plan === 2 ? "f-seg-text-on" : ""}`}>Invoice</Text>
          </View>
        </View>

        <View className="f-rows">
          <View className="f-row">
            <View className="f-row-text">
              <Text className="f-row-title">Weekly digest</Text>
              <Text className="f-help">A summary every Monday morning.</Text>
            </View>
            <View className="f-switch f-switch-on"><View className="f-knob f-knob-on" /></View>
          </View>
          <View className="f-row">
            <View className="f-row-text">
              <Text className="f-row-title">Two-step sign in</Text>
              <Text className="f-help">Recommended for every admin.</Text>
            </View>
            <View className="f-switch"><View className="f-knob" /></View>
          </View>
          <View className="f-check-row">
            <View className="f-check f-check-on"><Text className="f-check-mark">x</Text></View>
            <Text className="f-check-label">I agree to the terms and the data processing addendum</Text>
          </View>
        </View>

        <View className="f-rule" />

        <View className="f-foot">
          <Button label="Delete draft" variant="ghost" disabled={true} onPress={() => setSubmits(submits + 1)}>
            <Text>Delete draft</Text>
          </Button>
          <View className="f-spacer" />
          <Button label="Cancel" variant="secondary" onPress={() => setSubmits(submits + 1)}>
            <Text>Cancel</Text>
          </Button>
          <Button label="Create workspace" variant="primary" onPress={() => setSubmits(submits + 1)}>
            <Text>Create workspace</Text>
          </Button>
        </View>
      </View>
    </View>
  );
}
