// SPDX-License-Identifier: MIT

// The input controls gallery page (issue #240, ADR-047 item 10): Switch, Checkbox and RadioGroup in
// every state (off, on, mixed, disabled). The page keeps the state a caller keeps, so the same
// controlled contract the library documents is the one the golden and the runtime test exercise.
// The host switches theme, density and direction; the page holds no appearance logic.

import { useState } from "tessera";
import { Switch } from "./Switch";
import { Checkbox } from "./Checkbox";
import { RadioGroup } from "./RadioGroup";
import { Radio } from "./Radio";

export function ControlsPage() {
  const [wifi, setWifi] = useState<boolean>(false);
  const [sound, setSound] = useState<boolean>(true);
  const [terms, setTerms] = useState<boolean>(false);
  const [news, setNews] = useState<boolean>(true);
  const [plan, setPlan] = useState<int>(1);
  return (
    <View className="g-page">
      <Text className="g-title">Switch, Checkbox, Radio</Text>
      <View className="g-row">
        <Switch label="Wi-Fi" checked={wifi} onToggle={() => setWifi(!wifi)}>
          <Text>Wi-Fi</Text>
        </Switch>
        <Switch label="Sound" checked={sound} onToggle={() => setSound(!sound)}>
          <Text>Sound</Text>
        </Switch>
        <Switch label="Locked off" disabled={true} onToggle={() => setWifi(!wifi)}>
          <Text>Locked off</Text>
        </Switch>
        <Switch label="Locked on" checked={true} disabled={true} onToggle={() => setWifi(!wifi)}>
          <Text>Locked on</Text>
        </Switch>
      </View>
      <View className="g-row">
        <Checkbox label="Accept terms" checked={terms} onToggle={() => setTerms(!terms)}>
          <Text>Accept terms</Text>
        </Checkbox>
        <Checkbox label="Newsletter" checked={news} onToggle={() => setNews(!news)}>
          <Text>Newsletter</Text>
        </Checkbox>
        <Checkbox label="Select all" indeterminate={true} onToggle={() => setNews(!news)}>
          <Text>Select all</Text>
        </Checkbox>
        <Checkbox label="Locked" checked={true} disabled={true} onToggle={() => setNews(!news)}>
          <Text>Locked</Text>
        </Checkbox>
      </View>
      <RadioGroup label="Plan">
        <Radio label="Free" selected={plan === 0} onSelect={() => setPlan(0)}>
          <Text>Free</Text>
        </Radio>
        <Radio label="Team" selected={plan === 1} onSelect={() => setPlan(1)}>
          <Text>Team</Text>
        </Radio>
        <Radio label="Enterprise" selected={plan === 2} onSelect={() => setPlan(2)}>
          <Text>Enterprise</Text>
        </Radio>
        <Radio label="Legacy" disabled={true} onSelect={() => setPlan(3)}>
          <Text>Legacy (disabled)</Text>
        </Radio>
      </RadioGroup>
      <Text className="g-note">Plan {plan}</Text>
    </View>
  );
}
