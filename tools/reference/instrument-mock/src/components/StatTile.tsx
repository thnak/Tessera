// SPDX-License-Identifier: MIT

// A key figure on an LCD (the WELL recipe). Parts: `tstat` (root), `tstat-label` (micro),
// `tstat-value` (mono, tabular, glowing), `tstat-unit`, `tstat-delta` (`tstat-delta-good|bad|flat`,
// an LED plus an arrow glyph so the trend is not colour-only), `tstat-meter` + `tstat-meter-fill`
// (a segmented LED bar, 0..1).
import "./stat.css";

export type Trend = "up" | "down" | "flat";

export type StatTileProps = {
  label: string;
  value: string;
  unit?: string;
  delta?: string;
  trend?: Trend;
  // Whether "up" is good (throughput) or bad (latency, errors): picks the LED colour.
  upIsGood?: boolean;
  meter?: number;
};

const arrows: Record<Trend, string> = { up: "▲", down: "▼", flat: "▬" };

export function StatTile({ label, value, unit, delta, trend = "flat", upIsGood = true, meter }: StatTileProps) {
  const good = trend === "flat" ? "flat" : (trend === "up") === upIsGood ? "good" : "bad";
  const led = good === "good" ? "m-led-ok" : good === "bad" ? "m-led-warn" : "";
  return (
    <div className="tstat m-well">
      <span className="tstat-label m-micro">{label}</span>
      <span className="tstat-figure">
        <span className="tstat-value mono m-glow">{value}</span>
        {unit ? <span className="tstat-unit mono">{unit}</span> : null}
      </span>
      {delta ? (
        <span className={`tstat-delta tstat-delta-${good} mono`}>
          <span className={`m-led ${led}`} aria-hidden="true" />
          <span aria-hidden="true">{arrows[trend]}</span> {delta}
        </span>
      ) : null}
      {meter !== undefined ? (
        <span className="tstat-meter" aria-hidden="true">
          <span className="tstat-meter-fill" style={{ width: `${Math.round(meter * 100)}%` }} />
        </span>
      ) : null}
    </div>
  );
}
