// SPDX-License-Identifier: MIT

// A horizontal slider with the Slider primitive's keyboard contract (arrows +-step, PageUp/Down
// +-10 steps, Home/End, clamped) plus pointer drag, which the Tessera primitive cannot do yet (a
// handler cannot read layout bounds). Parts: `tslider` (root), `tslider-header`, `tslider-label`,
// `tslider-value` (mono readout), `tslider-control` (the focusable role=slider), `tslider-track`,
// `tslider-fill`, `tslider-thumb`.
import { useId, useRef, type KeyboardEvent, type PointerEvent } from "react";
import "./slider.css";

export type SliderProps = {
  label: string;
  value: number;
  onChange: (value: number) => void;
  min?: number;
  max?: number;
  step?: number;
  unit?: string;
  disabled?: boolean;
};

function clamp(v: number, lo: number, hi: number) {
  return Math.min(hi, Math.max(lo, v));
}

export function Slider({
  label,
  value,
  onChange,
  min = 0,
  max = 100,
  step = 1,
  unit = "",
  disabled = false,
}: SliderProps) {
  const id = useId();
  const trackRef = useRef<HTMLSpanElement>(null);
  const fraction = (value - min) / (max - min);

  const setFromPointer = (clientX: number) => {
    const rect = trackRef.current?.getBoundingClientRect();
    if (!rect || rect.width === 0) return;
    const raw = min + ((clientX - rect.left) / rect.width) * (max - min);
    onChange(clamp(Math.round(raw / step) * step, min, max));
  };

  const onKeyDown = (e: KeyboardEvent) => {
    const moves: Record<string, number> = {
      ArrowRight: value + step,
      ArrowUp: value + step,
      ArrowLeft: value - step,
      ArrowDown: value - step,
      PageUp: value + step * 10,
      PageDown: value - step * 10,
      Home: min,
      End: max,
    };
    if (e.key in moves) {
      e.preventDefault();
      onChange(clamp(moves[e.key], min, max));
    }
  };

  const onPointerDown = (e: PointerEvent) => {
    if (disabled) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    setFromPointer(e.clientX);
  };

  return (
    <div className={`tslider ${disabled ? "tslider-disabled" : ""}`}>
      <div className="tslider-header">
        <span className="tslider-label m-micro" id={id}>
          {label}
        </span>
        <span className="tslider-value m-well m-glow mono">
          {value}
          {unit}
        </span>
      </div>
      <div
        className="tslider-control"
        role="slider"
        tabIndex={disabled ? -1 : 0}
        aria-labelledby={id}
        aria-valuenow={value}
        aria-valuemin={min}
        aria-valuemax={max}
        aria-valuetext={`${value}${unit}`}
        aria-disabled={disabled || undefined}
        onKeyDown={disabled ? undefined : onKeyDown}
        onPointerDown={onPointerDown}
        onPointerMove={(e) => e.currentTarget.hasPointerCapture(e.pointerId) && setFromPointer(e.clientX)}
      >
        <span className="tslider-track m-well" ref={trackRef}>
          <span className="tslider-fill" style={{ width: `${fraction * 100}%` }} />
          <span className="tslider-thumb" style={{ left: `${fraction * 100}%` }} />
        </span>
      </div>
    </div>
  );
}
