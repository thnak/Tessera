// SPDX-License-Identifier: MIT

// Fixed sample data. Nothing here reads a clock or a random source, so every screenshot of the mock
// is reproducible.

export type RangeId = "1h" | "24h" | "7d";

export type Series = { values: number[]; xLabels: string[]; max: number; limit: number };

// A deterministic wave: the same index always gives the same value.
function wave(count: number, base: number, amplitude: number, phase: number): number[] {
  return Array.from({ length: count }, (_, i) =>
    Math.round(base + amplitude * Math.sin((i + phase) / 2.3) + (amplitude / 3) * Math.cos(i * 1.7)),
  );
}

export const series: Record<RangeId, Series> = {
  "1h": {
    values: wave(30, 52, 22, 0),
    xLabels: ["-60m", "-45m", "-30m", "-15m", "now"],
    max: 100,
    limit: 70,
  },
  "24h": {
    values: wave(24, 44, 26, 3),
    xLabels: ["00:00", "06:00", "12:00", "18:00", "24:00"],
    max: 100,
    limit: 70,
  },
  "7d": {
    values: wave(28, 58, 18, 7),
    xLabels: ["Mon", "Wed", "Fri", "Sun"],
    max: 100,
    limit: 70,
  },
};

export type Capture = {
  id: string;
  channel: string;
  status: string;
  samples: number;
  rate: number;
  dropped: number;
  duration: string;
};

export const captures: Capture[] = [
  { id: "CAP-0142", channel: "bus-a/pressure", status: "running", samples: 1284500, rate: 48000, dropped: 0, duration: "00:26:45" },
  { id: "CAP-0141", channel: "bus-a/temp", status: "running", samples: 96320, rate: 1000, dropped: 2, duration: "01:36:20" },
  { id: "CAP-0140", channel: "bus-b/strain-04", status: "stalled", samples: 410112, rate: 12000, dropped: 318, duration: "00:34:10" },
  { id: "CAP-0139", channel: "bus-b/strain-03", status: "done", samples: 2880000, rate: 12000, dropped: 0, duration: "04:00:00" },
  { id: "CAP-0138", channel: "bus-c/accel-x", status: "failed", samples: 1024, rate: 96000, dropped: 1024, duration: "00:00:01" },
  { id: "CAP-0137", channel: "bus-c/accel-y", status: "done", samples: 5760000, rate: 96000, dropped: 12, duration: "00:01:00" },
  { id: "CAP-0136", channel: "bus-a/pressure", status: "done", samples: 172800000, rate: 48000, dropped: 0, duration: "01:00:00" },
];

export const stats = [
  { label: "Throughput", value: "48.2", unit: "MB/s", delta: "+6.1% vs prev.", trend: "up", upIsGood: true },
  { label: "P99 latency", value: "3.84", unit: "ms", delta: "+0.42 ms", trend: "up", upIsGood: false },
  { label: "Dropped frames", value: "1,356", unit: "", delta: "-12 vs prev.", trend: "down", upIsGood: false },
  { label: "Buffer use", value: "72", unit: "%", delta: "steady", trend: "flat", upIsGood: true, meter: 0.72 },
] as const;

export function formatInt(n: number): string {
  return n.toLocaleString("en-US");
}
