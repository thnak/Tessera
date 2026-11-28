// SPDX-License-Identifier: MIT

// `ProgressBar`, the styled determinate progress of the Instrument library (issue #1041, ADR-059
// group E, the target of `progress`). It composes the `Progress` behavior primitive
// (primitives/Progress.tsx, role `progressbar` with `aria-valuenow/min/max`) and draws the fill
// with flex-grow shares of the same value, so no width is measured. `value` and `max` are
// integers and `max` defaults to 100. Parts: `tprg-track`, `tprg-fill`, `tprg-rest`, from
// `progress.css`.

import { Progress } from "./primitives/Progress";

export type ProgressBarProps = { label: string; value?: int; max?: int };

export function ProgressBar({ label, value = 0, max = 100 }: ProgressBarProps) {
  return (
    <Progress label={label} value={value} max={max}>
      <View className="tprg-track">
        <View className="tprg-fill" style={{ flexGrow: clamp(value, 0, max) }}></View>
        <View className="tprg-rest" style={{ flexGrow: clamp(max - value, 0, max) }}></View>
      </View>
    </Progress>
  );
}
