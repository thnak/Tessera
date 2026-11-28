// SPDX-License-Identifier: MIT

// `Meter`, the styled scalar gauge of the Instrument library (issue #1041, ADR-059 group E, the
// target of `meter`). It composes the `Progress` behavior primitive for the `progressbar`
// semantics and colours the fill by where `value` falls against `low` and `high`: the region of
// the value (below low, between, above high) is compared with the region of `optimum`, the same
// region means `tmtr-good`, the neighbouring one `tmtr-warn` and the far one `tmtr-bad`. All values
// are integers; `low`, `high` and `optimum` default to 0, 100 and 50, so set them with a `min`
// or `max` that is not 0..100. Parts: `tmtr`, `tmtr-track`, `tmtr-fill`, `tmtr-rest`, from
// `progress.css`.

import { Progress } from "./primitives/Progress";

export type MeterProps = {
  label: string;
  value?: int;
  min?: int;
  max?: int;
  low?: int;
  high?: int;
  optimum?: int;
};

export function Meter({
  label,
  value = 0,
  min = 0,
  max = 100,
  low = 0,
  high = 100,
  optimum = 50,
}: MeterProps) {
  const gap = (value < low ? 0 : value > high ? 2 : 1) - (optimum < low ? 0 : optimum > high ? 2 : 1);
  return (
    <Progress label={label} value={value} min={min} max={max}>
      <View
        className={gap === 0 ? "tmtr tmtr-good" : gap === 1 || gap === -1 ? "tmtr tmtr-warn" : "tmtr tmtr-bad"}
      >
        <View className="tmtr-track">
          <View className="tmtr-fill" style={{ flexGrow: clamp(value - min, 0, max - min) }}></View>
          <View className="tmtr-rest" style={{ flexGrow: clamp(max - value, 0, max - min) }}></View>
        </View>
      </View>
    </Progress>
  );
}
