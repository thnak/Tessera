// SPDX-License-Identifier: MIT

// The bottom band: a dark tab rising from the device's bottom edge (the app bar mirrored), short mono
// facts with LEDs, separated by engraved seams. Items marked `secondary` drop below the sm breakpoint.
// Parts: `tstatusbar`, `tstatusbar-item` (`tstatusbar-secondary`), `tstatusbar-spacer`.
import "./shell.css";

export type StatusItem = { text: string; dot?: "ok" | "warn" | "error"; secondary?: boolean; end?: boolean };

const dots = { ok: "m-led-ok", warn: "m-led-warn", error: "m-led-warn" };

export function StatusBar({ items }: { items: StatusItem[] }) {
  const start = items.filter((i) => !i.end);
  const end = items.filter((i) => i.end);
  const render = (i: StatusItem) => (
    <span key={i.text} className={`tstatusbar-item ${i.secondary ? "tstatusbar-secondary" : ""}`}>
      {i.dot ? <span className={`m-led ${dots[i.dot]}`} aria-hidden="true" /> : null}
      {i.text}
    </span>
  );
  return (
    <footer className="tstatusbar m-inverse m-micro" role="status" aria-live="polite">
      {start.map(render)}
      <span className="tstatusbar-spacer" />
      {end.map(render)}
    </footer>
  );
}
