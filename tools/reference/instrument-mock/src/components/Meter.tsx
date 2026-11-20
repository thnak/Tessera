// SPDX-License-Identifier: MIT

// A two-channel level meter: segmented LED columns in an LCD well, signal colour below the warn
// threshold and warning colour above it (the reference's L/R output meter). Read-only; exposed as a
// meter with a text value. Parts: `tmeter`, `tmeter-well`, `tmeter-col`, `tmeter-fill`,
// `tmeter-hot`, `tmeter-labels`.
import type { CSSProperties } from "react";
import "./meter.css";

export type MeterProps = {
  label: string;
  left: number;
  right: number;
  hot?: number;
};

export function Meter({ label, left, right, hot = 0.8 }: MeterProps) {
  const col = (v: number) =>
    ({ "--level": `${Math.round(v * 100)}%`, "--hot": `${Math.round(hot * 100)}%` }) as CSSProperties;
  const peak = Math.round(Math.max(left, right) * 100);
  return (
    <div className="tmeter">
      <div
        className="tmeter-well m-well"
        role="meter"
        aria-label={label}
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={peak}
        aria-valuetext={`${label}: left ${Math.round(left * 100)}%, right ${Math.round(right * 100)}%`}
      >
        <span className="tmeter-col" style={col(left)} aria-hidden="true">
          <span className="tmeter-fill" />
        </span>
        <span className="tmeter-col" style={col(right)} aria-hidden="true">
          <span className="tmeter-fill" />
        </span>
      </div>
      <div className="tmeter-labels m-micro" aria-hidden="true">
        <span>L</span>
        <span>R</span>
      </div>
    </div>
  );
}
