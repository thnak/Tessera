// SPDX-License-Identifier: MIT

// A rotary control with the Slider primitive's contract (role=slider; arrows +-step, PageUp/Down
// +-10 steps, Home/End, clamped) plus vertical pointer drag. 270 degrees of travel from 7:30 to 4:30.
// `lg` is the reference's light knurled knob with a tick ring and glowing arc; `sm` is the dark macro
// knob with a segmented arc and a centre LED, for the inverse band. Geometry reaches CSS only as two
// custom properties (--deg, --rot); every drawing technique lives in knob.css.
// Parts: `tknob` (root), `tknob-dial` (focusable), `tknob-ticks`, `tknob-ticks-on`, `tknob-arc`,
// `tknob-knurl`, `tknob-cap`, `tknob-pointer`, `tknob-led`, `tknob-min`, `tknob-max`,
// `tknob-caption`, `tknob-label`, `tknob-value`.
import { useId, useRef, type CSSProperties, type KeyboardEvent, type PointerEvent } from "react";
import "./knob.css";

export type KnobProps = {
  label: string;
  value: number;
  onChange: (value: number) => void;
  min?: number;
  max?: number;
  step?: number;
  size?: "lg" | "sm";
  format?: (value: number) => string;
  ends?: [string, string];
};

function clamp(v: number, lo: number, hi: number) {
  return Math.min(hi, Math.max(lo, v));
}

export function Knob({
  label,
  value,
  onChange,
  min = 0,
  max = 100,
  step = 1,
  size = "lg",
  format = (v) => String(v),
  ends = ["MIN", "MAX"],
}: KnobProps) {
  const id = useId();
  const drag = useRef<{ y: number; v: number } | null>(null);
  const fraction = (value - min) / (max - min);
  const geometry = { "--deg": `${fraction * 270}deg`, "--rot": `${fraction * 270 - 135}deg` } as CSSProperties;

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

  // Drag up to increase: 160px of travel covers the whole range.
  const onPointerDown = (e: PointerEvent) => {
    e.currentTarget.setPointerCapture(e.pointerId);
    drag.current = { y: e.clientY, v: value };
  };
  const onPointerMove = (e: PointerEvent) => {
    if (!drag.current) return;
    const raw = drag.current.v + ((drag.current.y - e.clientY) / 160) * (max - min);
    onChange(clamp(Math.round(raw / step) * step, min, max));
  };

  return (
    <div className={`tknob tknob-${size}`} style={geometry}>
      <div
        className="tknob-dial"
        role="slider"
        tabIndex={0}
        aria-labelledby={id}
        aria-valuenow={value}
        aria-valuemin={min}
        aria-valuemax={max}
        aria-valuetext={format(value)}
        onKeyDown={onKeyDown}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={() => (drag.current = null)}
      >
        {size === "lg" ? (
          <>
            <span className="tknob-ticks" aria-hidden="true" />
            <span className="tknob-ticks-on" aria-hidden="true" />
            <span className="tknob-arc-glow" aria-hidden="true">
              <span className="tknob-arc" />
            </span>
            <span className="tknob-knurl" aria-hidden="true" />
            <span className="tknob-cap" aria-hidden="true" />
            <span className="tknob-pointer" aria-hidden="true" />
            <span className="tknob-min" aria-hidden="true">{ends[0]}</span>
            <span className="tknob-max" aria-hidden="true">{ends[1]}</span>
          </>
        ) : (
          <>
            <span className="tknob-arc-glow" aria-hidden="true">
              <span className="tknob-arc" />
            </span>
            <span className="tknob-cap" aria-hidden="true" />
            <span className="tknob-knurl" aria-hidden="true" />
            <span className="tknob-led" aria-hidden="true" />
            <span className="tknob-pointer" aria-hidden="true" />
          </>
        )}
      </div>
      <div className="tknob-caption">
        <span className="tknob-label" id={id}>
          {label}
        </span>
        <span className={`tknob-value mono ${size === "lg" ? "m-well m-glow" : ""}`}>{format(value)}</span>
      </div>
    </div>
  );
}
