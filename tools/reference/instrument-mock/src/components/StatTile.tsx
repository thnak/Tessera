// SPDX-License-Identifier: MIT

// A key figure. Parts: `tstat` (root), `tstat-label`, `tstat-value` (mono, tabular), `tstat-unit`,
// `tstat-delta` (`tstat-delta-up|down|flat`, with an arrow glyph so the trend is not colour-only),
// `tstat-meter` + `tstat-meter-fill` (optional 0..1 bar).
import "./stat.css";

export type Trend = "up" | "down" | "flat";

export type StatTileProps = {
  label: string;
  value: string;
  unit?: string;
  delta?: string;
  trend?: Trend;
  // Whether "up" is good (throughput) or bad (latency, errors): picks success or danger.
  upIsGood?: boolean;
  meter?: number;
};

const arrows: Record<Trend, string> = { up: "▲", down: "▼", flat: "▬" };

export function StatTile({ label, value, unit, delta, trend = "flat", upIsGood = true, meter }: StatTileProps) {
  const good = trend === "flat" ? "flat" : (trend === "up") === upIsGood ? "good" : "bad";
  return (
    <div className="tstat">
      <span className="tstat-label">{label}</span>
      <span className="tstat-figure">
        <span className="tstat-value mono">{value}</span>
        {unit ? <span className="tstat-unit mono">{unit}</span> : null}
      </span>
      {delta ? (
        <span className={`tstat-delta tstat-delta-${good} mono`}>
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
