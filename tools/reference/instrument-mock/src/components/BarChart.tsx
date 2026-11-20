// SPDX-License-Identifier: MIT

// A column chart from plain boxes (no SVG, no canvas), so it maps onto Tessera views one to one.
// Bars over a limit are drawn in the warning colour and carry a top notch, so the state is not
// colour-only. Parts: `tchart`, `tchart-axis-y`, `tchart-tick`, `tchart-plot`, `tchart-grid`,
// `tchart-limit`, `tchart-bar` (`tchart-bar-over`), `tchart-axis-x`, `tchart-legend`.
import "./chart.css";

export type BarChartProps = {
  label: string;
  values: number[];
  xLabels: string[];
  max: number;
  limit?: number;
  unit: string;
};

const ticks = [1, 0.75, 0.5, 0.25, 0];

export function BarChart({ label, values, xLabels, max, limit, unit }: BarChartProps) {
  const peak = Math.max(...values);
  const summary = `${label}: ${values.length} samples, peak ${peak} ${unit}` +
    (limit !== undefined ? `, ${values.filter((v) => v > limit).length} over the ${limit} ${unit} limit` : "");
  return (
    <figure className="tchart">
      <div className="tchart-body" role="img" aria-label={summary}>
        <div className="tchart-axis-y mono" aria-hidden="true">
          {ticks.map((t) => (
            <span key={t} className="tchart-tick">
              {Math.round(max * t)}
            </span>
          ))}
        </div>
        <div className="tchart-plot">
          {ticks.map((t) => (
            <span key={t} className="tchart-grid" style={{ bottom: `${t * 100}%` }} />
          ))}
          {limit !== undefined ? (
            <span className="tchart-limit" style={{ bottom: `${(limit / max) * 100}%` }}>
              <span className="tchart-limit-text mono">
                limit {limit}
              </span>
            </span>
          ) : null}
          <div className="tchart-bars">
            {values.map((v, i) => (
              <span
                key={i}
                className={`tchart-bar ${limit !== undefined && v > limit ? "tchart-bar-over" : ""}`}
                style={{ height: `${(v / max) * 100}%` }}
              />
            ))}
          </div>
        </div>
      </div>
      <div className="tchart-axis-x mono" aria-hidden="true">
        {xLabels.map((l, i) => (
          <span key={i} className="tchart-xlabel">
            {l}
          </span>
        ))}
      </div>
      <figcaption className="tchart-legend">
        <span className="tchart-key">
          <span className="tchart-swatch" aria-hidden="true" /> {unit}
        </span>
        {limit !== undefined ? (
          <span className="tchart-key">
            <span className="tchart-swatch tchart-swatch-over" aria-hidden="true" /> over limit
          </span>
        ) : null}
      </figcaption>
    </figure>
  );
}
