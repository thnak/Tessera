// SPDX-License-Identifier: MIT

// A titled region: hairline frame, flat raised header, no shadow. Parts: `tpanel`, `tpanel-header`,
// `tpanel-title`, `tpanel-meta` (mono, right-aligned), `tpanel-body`.
import { useId, type ReactNode } from "react";
import "./panel.css";

export type PanelProps = {
  title: string;
  meta?: ReactNode;
  flush?: boolean;
  children: ReactNode;
};

export function Panel({ title, meta, flush = false, children }: PanelProps) {
  const id = useId();
  return (
    <section className="tpanel" aria-labelledby={id}>
      <header className="tpanel-header">
        <h2 className="tpanel-title" id={id}>
          {title}
        </h2>
        {meta ? <span className="tpanel-meta mono">{meta}</span> : null}
      </header>
      <div className={`tpanel-body ${flush ? "tpanel-body-flush" : ""}`}>{children}</div>
    </section>
  );
}
