// SPDX-License-Identifier: MIT

// Dashboard: key figures, a throughput chart per time range (tabs), and the capture table.
import { useState } from "react";
import { Badge, type BadgeTone } from "../components/Badge";
import { BarChart } from "../components/BarChart";
import { Button } from "../components/Button";
import { DataTable, type Column } from "../components/DataTable";
import { Panel } from "../components/Panel";
import { StatTile } from "../components/StatTile";
import { Tabs } from "../components/Tabs";
import { Console } from "./Console";
import { captures, formatInt, series, stats, type Capture, type RangeId } from "../data";
import "./page.css";

const tones: Record<string, BadgeTone> = {
  running: "accent",
  done: "success",
  stalled: "warning",
  failed: "danger",
};

const columns: Column<Capture>[] = [
  { key: "id", label: "Capture", render: (r) => <span className="mono">{r.id}</span> },
  { key: "channel", label: "Channel" },
  { key: "status", label: "Status", render: (r) => <Badge tone={tones[r.status]}>{r.status}</Badge> },
  { key: "samples", label: "Samples", numeric: true, render: (r) => formatInt(r.samples), hideNarrow: true },
  { key: "rate", label: "Rate Hz", numeric: true, render: (r) => formatInt(r.rate), hideNarrow: true },
  { key: "dropped", label: "Dropped", numeric: true, render: (r) => formatInt(r.dropped) },
  { key: "duration", label: "Duration", numeric: true, hideNarrow: true },
];

export function Dashboard({ onNewCapture }: { onNewCapture: () => void }) {
  const [range, setRange] = useState<RangeId>("1h");
  const data = series[range];
  return (
    <div className="page">
      <div className="page-header">
        <div className="page-heading">
          <span className="m-micro">Monitor / Bench 3</span>
          <h1 className="page-title m-display">Overview</h1>
          <p className="page-subtitle">4 channels · last sync 12:04:31 UTC</p>
        </div>
        <div className="page-actions">
          <Button variant="secondary">Export CSV</Button>
          <Button variant="dark" onClick={onNewCapture}>
            <span className="m-led m-led-warn" aria-hidden="true" />
            New capture
          </Button>
        </div>
      </div>

      <Console />

      <div className="tiles">
        {stats.map((s) => (
          <StatTile key={s.label} {...s} />
        ))}
      </div>

      <Tabs
        label="Time range"
        selected={range}
        onSelect={(id) => setRange(id as RangeId)}
        tabs={[
          { id: "1h", label: "Last hour" },
          { id: "24h", label: "24 hours" },
          { id: "7d", label: "7 days" },
        ]}
      >
        <Panel index="04" title="Throughput" meta={`${data.values.length} samples · MB/s`}>
          <BarChart
            label="Throughput"
            values={data.values}
            xLabels={data.xLabels}
            max={data.max}
            limit={data.limit}
            unit="MB/s"
          />
        </Panel>
      </Tabs>

      <Panel index="05" title="Recent captures" meta={`${captures.length} rows`} flush>
        <DataTable caption="Recent captures" columns={columns} rows={captures} rowKey={(r) => r.id} />
      </Panel>
    </div>
  );
}
