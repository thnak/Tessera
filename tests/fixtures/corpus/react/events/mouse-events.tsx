// SPDX-License-Identifier: MIT

// Corpus fixture: `onMouseEnter` and `onDoubleClick`. ADR-066 section 3 keeps these rejected (Q21):
// they fire for a mouse only, so the port uses the pointer events or a behavior hook.

export function HoverCard({ label }: { label: string }) {
  return (
    <div className="hover-card" onMouseEnter={() => {}} onDoubleClick={() => {}}>
      <span className="hover-label">{label}</span>
    </div>
  );
}
