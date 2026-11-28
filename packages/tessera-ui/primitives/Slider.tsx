// SPDX-License-Identifier: MIT

// `Slider`, the slider-value behavior primitive (issue #236). It draws nothing: one focusable
// `slider` View that owns an int value in `[min, max]` and exposes it as `aria-valuenow`,
// `aria-valuemin` and `aria-valuemax`. Right/Up add `step`, Left/Down subtract it, PageUp/PageDown
// move ten steps, Home and End jump to the bounds; the value is clamped.
//
// Known gap: pointer dragging is not wired here yet. It is unblocked (issue #532): a pointer handler
// reads its node-local `e.x` and the track width stored from `onSizeChange`
// (docs/specs/jsx-intrinsics-v1/common-and-events.md section 3.1); the styled Slider adds it.

import { Children, KeyEvent, useState } from "tessera";

export type SliderProps = {
  label: string;
  min?: int;
  max?: int;
  step?: int;
  defaultValue?: int;
  children: Children;
};

export function Slider({
  label,
  min = 0,
  max = 100,
  step = 1,
  defaultValue = 0,
  children,
}: SliderProps) {
  const [value, setValue] = useState<int>(defaultValue);
  return (
    <View
      role="slider"
      tabIndex={0}
      aria-label={label}
      aria-valuenow={value}
      aria-valuemin={min}
      aria-valuemax={max}
      onKeyDown={(e: KeyEvent) => {
        (e.key === "ArrowRight" || e.key === "ArrowUp") && setValue(clamp(value + step, min, max));
        (e.key === "ArrowLeft" || e.key === "ArrowDown") && setValue(clamp(value - step, min, max));
        e.key === "PageUp" && setValue(clamp(value + step * 10, min, max));
        e.key === "PageDown" && setValue(clamp(value - step * 10, min, max));
        e.key === "Home" && setValue(min);
        e.key === "End" && setValue(max);
      }}
    >
      {children}
    </View>
  );
}
