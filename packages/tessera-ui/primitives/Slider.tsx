// SPDX-License-Identifier: MIT

// `Slider`, the slider-value behavior primitive (issue #236). It draws nothing: one focusable
// `slider` View that owns an int value in `[min, max]` and exposes it as `aria-valuenow`,
// `aria-valuemin` and `aria-valuemax`. Right/Up add `step`, Left/Down subtract it, PageUp/PageDown
// move ten steps, Home and End jump to the bounds; the value is clamped.
//
// Known gap: pointer dragging is not here. A handler cannot read the node's layout bounds, so a
// pointer position cannot become a value in the JSX subset yet; the styled Slider needs that.

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
