// SPDX-License-Identifier: MIT

// A status label: a small square marker plus text, so status is never colour-only. Parts: `tbadge`
// (`tbadge-neutral|accent|success|warning|danger`), `tbadge-dot`, `tbadge-text`.
import "./badge.css";

export type BadgeTone = "neutral" | "accent" | "success" | "warning" | "danger";

export function Badge({ tone = "neutral", children }: { tone?: BadgeTone; children: string }) {
  return (
    <span className={`tbadge tbadge-${tone}`}>
      <span className="tbadge-dot" aria-hidden="true" />
      <span className="tbadge-text">{children}</span>
    </span>
  );
}
