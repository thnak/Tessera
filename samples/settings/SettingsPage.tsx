// SPDX-License-Identifier: MIT

// The Settings starter template (issues #240 and #527, docs/design/component-library.md section 8,
// stage 1). It is built only from public library components (Switch, Checkbox, RadioGroup, Radio,
// ProgressBar, Details, Button) and the intrinsic View and Text, so the screen measures what a real
// settings page needs today. What it had to fake with plain Views is the gap list in README.md.
//
// The page keeps the state; every control is the controlled contract the library documents. The
// host picks theme, density, text scale and direction, so the page holds no appearance logic.

import { useState } from "tessera";
import { Button } from "./Button";
import { Switch } from "./Switch";
import { Checkbox } from "./Checkbox";
import { RadioGroup } from "./RadioGroup";
import { Radio } from "./Radio";
import { ProgressBar } from "./ProgressBar";
import { Details } from "./Details";

export function SettingsPage() {
  const [email, setEmail] = useState<boolean>(true);
  const [push, setPush] = useState<boolean>(false);
  const [digest, setDigest] = useState<boolean>(true);
  const [motion, setMotion] = useState<boolean>(false);
  const [density, setDensity] = useState<int>(0);
  const [telemetry, setTelemetry] = useState<boolean>(false);
  const [edits, setEdits] = useState<int>(0);
  const [saves, setSaves] = useState<int>(0);
  return (
    <View className="s-page">
      <View className="s-card">
        <View className="s-head">
          <Text className="s-title">Settings</Text>
          <Text className="s-sub">Changes apply when you save.</Text>
        </View>

        <View className="s-section">
          <Text className="s-heading">Notifications</Text>
          <View className="s-row">
            <Switch
              label="Email notifications"
              checked={email}
              onToggle={() => {
                setEmail(!email);
                setEdits(edits + 1);
              }}
            >
              <Text>Email notifications</Text>
            </Switch>
          </View>
          <View className="s-row">
            <Switch
              label="Push notifications"
              checked={push}
              onToggle={() => {
                setPush(!push);
                setEdits(edits + 1);
              }}
            >
              <Text>Push notifications</Text>
            </Switch>
          </View>
          <View className="s-row">
            <Checkbox
              label="Weekly digest"
              checked={digest}
              onToggle={() => {
                setDigest(!digest);
                setEdits(edits + 1);
              }}
            >
              <Text>Weekly digest</Text>
            </Checkbox>
          </View>
        </View>

        <View className="s-rule" />

        <View className="s-section">
          <Text className="s-heading">Appearance</Text>
          <RadioGroup label="Density">
            <Radio
              label="Comfortable"
              selected={density === 0}
              onSelect={() => {
                setDensity(0);
                setEdits(edits + 1);
              }}
            >
              <Text>Comfortable</Text>
            </Radio>
            <Radio
              label="Compact"
              selected={density === 1}
              onSelect={() => {
                setDensity(1);
                setEdits(edits + 1);
              }}
            >
              <Text>Compact</Text>
            </Radio>
          </RadioGroup>
          <View className="s-row">
            <Switch
              label="Reduce motion"
              checked={motion}
              onToggle={() => {
                setMotion(!motion);
                setEdits(edits + 1);
              }}
            >
              <Text>Reduce motion</Text>
            </Switch>
          </View>
        </View>

        <View className="s-rule" />

        <View className="s-section">
          <Text className="s-heading">Storage</Text>
          <ProgressBar label="Storage used" value={62} max={100} />
          <Text className="s-note">6.2 GB of 10 GB used</Text>
        </View>

        <View className="s-rule" />

        <Details summary="Advanced">
          <View className="s-row">
            <Checkbox
              label="Share diagnostics"
              checked={telemetry}
              onToggle={() => {
                setTelemetry(!telemetry);
                setEdits(edits + 1);
              }}
            >
              <Text>Share anonymous diagnostics</Text>
            </Checkbox>
          </View>
        </Details>

        <View className="s-foot">
          <Text className="s-note">
            {edits === saves ? "All changes saved" : "Unsaved changes"}
          </Text>
          <View className="s-spacer" />
          <Button label="Save" variant="primary" onPress={() => setSaves(edits)}>
            <Text>Save</Text>
          </Button>
        </View>
      </View>
    </View>
  );
}
