// SPDX-License-Identifier: MIT

// `Progress`, the progress and meter behavior primitive (issue #544, ADR-059 group E). It draws
// nothing: one `progressbar` View that exposes the caller's value and bounds as `aria-valuenow`,
// `aria-valuemin` and `aria-valuemax`, the contract `progress` and `meter` share. The value is a
// prop, not state, because progress is reported by whoever does the work; the children slot is
// where the styled component draws the fill, sized from the same value. It is not focusable.

import { Children } from "tessera";

export type ProgressProps = {
  label: string;
  value?: int;
  min?: int;
  max?: int;
  children: Children;
};

export function Progress({ label, value = 0, min = 0, max = 100, children }: ProgressProps) {
  return (
    <View
      role="progressbar"
      aria-label={label}
      aria-valuenow={clamp(value, min, max)}
      aria-valuemin={min}
      aria-valuemax={max}
    >
      {children}
    </View>
  );
}
