// SPDX-License-Identifier: MIT

// `Range`, the styled slider of the Instrument library (issue #1041, ADR-059 group E, the target
// of `input type="range"`). It keeps its integer value in `[min, max]` and exposes it as
// `aria-valuenow`, `aria-valuemin` and `aria-valuemax` on one focusable `slider`; arrows add
// `step`, PageUp/PageDown ten steps, Home/End jump to the bounds, a pointer press or drag maps the
// node-local `e.x` over the width to a step-snapped value (the rules of primitives/Slider.tsx).
//
// It restates the primitive's handlers instead of composing it: the primitive keeps the value in
// state its children cannot read, so a fill and a thumb could not follow it (#1051 tracks exposing
// it, with the controlled `value` the `input` tag needs). The state is the distance `moved` from
// `defaultValue`, not the value: a state initializer that reads a prop is evaluated before the
// parent has written its props, so `useState(defaultValue)` would always start at the prop's
// default (#1055). Parts: `trng-track`, `trng-fill`,
// `trng-thumb`, `trng-rest`, from `range.css`.

import { KeyEvent, PointerEvent, SizeChangeEvent, useState } from "tessera";

export type RangeProps = { label: string; min?: int; max?: int; step?: int; defaultValue?: int };

export function Range({ label, min = 0, max = 100, step = 1, defaultValue = 0 }: RangeProps) {
  const [moved, setMoved] = useState<int>(0);
  const [width, setWidth] = useState<float>(0);
  const [dragging, setDragging] = useState<boolean>(false);
  const value = clamp(defaultValue + moved, min, max);
  return (
    <View
      role="slider"
      tabIndex={0}
      aria-label={label}
      aria-valuenow={value}
      aria-valuemin={min}
      aria-valuemax={max}
      className="trng"
      onSizeChange={(e: SizeChangeEvent) => setWidth(e.width)}
      onPointerDown={(e: PointerEvent) => {
        setDragging(true);
        width > 0 &&
          setMoved(
            clamp(
              min + toInt(
                round((clamp(e.x / width, toFloat(0), toFloat(1)) * (max - min)) / step)
              ) * step,
              min,
              max
            ) - defaultValue
          );
      }}
      onPointerMove={(e: PointerEvent) =>
        dragging &&
          width > 0 &&
          setMoved(
            clamp(
              min + toInt(
                round((clamp(e.x / width, toFloat(0), toFloat(1)) * (max - min)) / step)
              ) * step,
              min,
              max
            ) - defaultValue
          )}
      onPointerUp={() => setDragging(false)}
      onKeyDown={(e: KeyEvent) => {
        (e.key === "ArrowRight" || e.key === "ArrowUp") &&
          setMoved(clamp(value + step, min, max) - defaultValue);
        (e.key === "ArrowLeft" || e.key === "ArrowDown") &&
          setMoved(clamp(value - step, min, max) - defaultValue);
        e.key === "PageUp" && setMoved(clamp(value + step * 10, min, max) - defaultValue);
        e.key === "PageDown" && setMoved(clamp(value - step * 10, min, max) - defaultValue);
        e.key === "Home" && setMoved(min - defaultValue);
        e.key === "End" && setMoved(max - defaultValue);
      }}
    >
      <View className="trng-track">
        <View className="trng-fill" style={{ flexGrow: clamp(value - min, 0, max - min) }}></View>
        <View className="trng-thumb"></View>
        <View className="trng-rest" style={{ flexGrow: clamp(max - value, 0, max - min) }}></View>
      </View>
    </View>
  );
}
