// SPDX-License-Identifier: MIT

// The HTML form-tag gallery page (issue #1041, ADR-059 group E): `input type="checkbox"`, `"radio"`
// and `"range"`, `progress`, `meter` and `details`/`summary`, written as lowercase tags. Each tag
// lowers onto the library component the file imports (docs/specs/jsx-intrinsics-v1/html-aliases.md
// section 3.8), so the page shows the same Checkbox, Radio, Range, ProgressBar, Meter and Details
// the library ships, in every state. The page keeps the state a caller keeps; the host switches
// theme and direction, and the page holds no appearance logic.

import { useState } from "tessera";
import { Checkbox } from "./Checkbox";
import { Radio } from "./Radio";
import { Range } from "./Range";
import { ProgressBar } from "./ProgressBar";
import { Meter } from "./Meter";
import { Details } from "./Details";

export function TagsPage() {
  const [terms, setTerms] = useState<boolean>(false);
  const [news, setNews] = useState<boolean>(true);
  const [plan, setPlan] = useState<int>(1);
  return (
    <View className="g-page">
      <Text className="g-title">input, progress, meter, details</Text>
      <View className="g-row">
        <input
          type="checkbox"
          aria-label="Accept terms"
          checked={terms}
          onChange={() => setTerms(!terms)}
        />
        <Text>Accept terms</Text>
        <input
          type="checkbox"
          aria-label="Newsletter"
          checked={news}
          onChange={() => setNews(!news)}
        />
        <Text>Newsletter</Text>
        <input type="checkbox" aria-label="Locked" checked={true} disabled />
        <Text>Locked</Text>
      </View>
      <View className="g-row">
        <input type="radio" aria-label="Free" checked={plan === 0} onChange={() => setPlan(0)} />
        <Text>Free</Text>
        <input type="radio" aria-label="Team" checked={plan === 1} onChange={() => setPlan(1)} />
        <Text>Team</Text>
        <input
          type="radio"
          aria-label="Enterprise"
          checked={plan === 2}
          onChange={() => setPlan(2)}
        />
        <Text>Enterprise</Text>
      </View>
      <input type="range" min={0} max={100} step={5} defaultValue={40} aria-label="Volume" />
      <input type="range" min={0} max={10} defaultValue={10} aria-label="Brightness" />
      <progress value={25} max={100} aria-label="Upload" />
      <progress value={70} max={100} aria-label="Download" />
      <progress value={100} max={100} aria-label="Done" />
      <meter value={50} min={0} max={100} low={25} high={75} optimum={50} aria-label="Battery" />
      <meter value={10} min={0} max={100} low={25} high={75} optimum={50} aria-label="Signal" />
      <meter value={90} min={0} max={100} low={25} high={75} optimum={10} aria-label="Disk" />
      <details open>
        <summary>Shipping</summary>
        <Text>Ships in two business days.</Text>
      </details>
      <details>
        <summary>Returns</summary>
        <Text>Thirty days, unused.</Text>
      </details>
      <Text className="g-note">Plan {plan}</Text>
    </View>
  );
}
