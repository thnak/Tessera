// SPDX-License-Identifier: MIT

// A titled region drawn as a plate (raised by tone and highlight, no box outline). The header is the
// reference's section head: a numbered index badge, a display title, an engraved rule that fades out,
// and an optional micro meta. Parts: `tpanel`, `tpanel-header`, `tpanel-index`, `tpanel-title`,
// `tpanel-rule`, `tpanel-meta`, `tpanel-body`; `screws` adds the four corner screws.
import { useId, type ReactNode } from "react";
import "./panel.css";

export type PanelProps = {
  title: string;
  index?: string;
  meta?: ReactNode;
  flush?: boolean;
  screws?: boolean;
  children: ReactNode;
};

export function Panel({ title, index, meta, flush = false, screws = false, children }: PanelProps) {
  const id = useId();
  return (
    <section className={`tpanel m-plate ${screws ? "tpanel-screwed" : ""}`} aria-labelledby={id}>
      {screws ? (
        <>
          <span className="m-screw m-screw-tl" aria-hidden="true" />
          <span className="m-screw m-screw-tr" aria-hidden="true" />
          <span className="m-screw m-screw-bl" aria-hidden="true" />
          <span className="m-screw m-screw-br" aria-hidden="true" />
        </>
      ) : null}
      <header className="tpanel-header">
        {index ? <span className="tpanel-index m-index">{index}</span> : null}
        <h2 className="tpanel-title m-display" id={id}>
          {title}
        </h2>
        <span className="tpanel-rule m-engrave" aria-hidden="true" />
        {meta ? <span className="tpanel-meta m-micro">{meta}</span> : null}
      </header>
      <div className={`tpanel-body ${flush ? "tpanel-body-flush" : ""}`}>{children}</div>
    </section>
  );
}
