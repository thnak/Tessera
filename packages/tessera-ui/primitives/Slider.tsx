// SPDX-License-Identifier: MIT

// `Slider`, the slider-value behavior primitive (issue #236). It draws nothing: one focusable
// `slider` View that owns an int value in `[min, max]` and exposes it as `aria-valuenow`,
// `aria-valuemin` and `aria-valuemax`. Right/Up add `step`, Left/Down subtract it, PageUp/PageDown
// move ten steps, Home and End jump to the bounds; the value is clamped.
//
// Pointer drag (issue #982, on #532): `onSizeChange` stores the track width, a press starts a drag
// and every move while it lasts maps the node-local `e.x` to a fraction of that width, snapped to
// `step` and clamped to `[min, max]`. A press captures its pointer implicitly, so `e.x` going below
// 0 or past the width still arrives and clamps; release ends the drag (`onPointerCancel` has no scene event type yet). Until the first layout the width is 0 and a pointer event changes nothing.

import { Children, KeyEvent, PointerEvent, SizeChangeEvent, useState } from "tessera";

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
  const [width, setWidth] = useState<float>(0);
  const [dragging, setDragging] = useState<boolean>(false);
  return (
    <View
      role="slider"
      tabIndex={0}
      aria-label={label}
      aria-valuenow={value}
      aria-valuemin={min}
      aria-valuemax={max}
      onSizeChange={(e: SizeChangeEvent) => setWidth(e.width)}
      onPointerDown={(e: PointerEvent) => {
        setDragging(true);
        width > 0 &&
          setValue(
            clamp(
              min + toInt(round((clamp(e.x / width, toFloat(0), toFloat(1)) * (max - min)) / step)) * step,
              min,
              max
            )
          );
      }}
      onPointerMove={(e: PointerEvent) =>
        dragging &&
          width > 0 &&
          setValue(
            clamp(
              min + toInt(round((clamp(e.x / width, toFloat(0), toFloat(1)) * (max - min)) / step)) * step,
              min,
              max
            )
          )}
      onPointerUp={() => setDragging(false)}
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
