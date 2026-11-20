// SPDX-License-Identifier: MIT

// The bottom status line: short mono facts separated by hairlines. Items marked `secondary` are
// dropped below the sm breakpoint. Parts: `tstatusbar`, `tstatusbar-item`
// (`tstatusbar-secondary`), `tstatusbar-dot` (`-ok|-warn|-error`), `tstatusbar-spacer`.
import "./shell.css";

export type StatusItem = { text: string; dot?: "ok" | "warn" | "error"; secondary?: boolean; end?: boolean };

export function StatusBar({ items }: { items: StatusItem[] }) {
  const start = items.filter((i) => !i.end);
  const end = items.filter((i) => i.end);
  const render = (i: StatusItem) => (
    <span key={i.text} className={`tstatusbar-item ${i.secondary ? "tstatusbar-secondary" : ""}`}>
      {i.dot ? <span className={`tstatusbar-dot tstatusbar-dot-${i.dot}`} aria-hidden="true" /> : null}
      {i.text}
    </span>
  );
  return (
    <footer className="tstatusbar mono" role="status" aria-live="polite">
      {start.map(render)}
      <span className="tstatusbar-spacer" />
      {end.map(render)}
    </footer>
  );
}
