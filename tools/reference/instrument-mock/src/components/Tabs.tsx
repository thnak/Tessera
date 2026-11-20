// SPDX-License-Identifier: MIT

// Tabs with automatic activation: one Tab stop, Left/Right (wrapping) and Home/End move and select,
// the RovingFocus primitive's contract. Parts: `ttabs-list`, `ttabs-tab` (selected adds
// `ttabs-tab-selected`), `ttabs-count` (optional mono badge), `ttabs-panel`.
import { useId, useRef, type KeyboardEvent, type ReactNode } from "react";
import "./tabs.css";

export type TabItem = { id: string; label: string; count?: number };

export type TabsProps = {
  label: string;
  tabs: TabItem[];
  selected: string;
  onSelect: (id: string) => void;
  children: ReactNode;
};

export function Tabs({ label, tabs, selected, onSelect, children }: TabsProps) {
  const base = useId();
  const listRef = useRef<HTMLDivElement>(null);

  const onKeyDown = (e: KeyboardEvent) => {
    const index = tabs.findIndex((t) => t.id === selected);
    const last = tabs.length - 1;
    const next: Record<string, number> = {
      ArrowRight: index === last ? 0 : index + 1,
      ArrowLeft: index === 0 ? last : index - 1,
      Home: 0,
      End: last,
    };
    if (!(e.key in next)) return;
    e.preventDefault();
    onSelect(tabs[next[e.key]].id);
    const buttons = listRef.current?.querySelectorAll<HTMLButtonElement>("[role=tab]");
    buttons?.[next[e.key]].focus();
  };

  return (
    <div className="ttabs">
      <div className="ttabs-list" role="tablist" aria-label={label} ref={listRef} onKeyDown={onKeyDown}>
        {tabs.map((t) => {
          const on = t.id === selected;
          return (
            <button
              key={t.id}
              type="button"
              role="tab"
              id={`${base}-tab-${t.id}`}
              aria-selected={on}
              aria-controls={`${base}-panel`}
              tabIndex={on ? 0 : -1}
              className={`ttabs-tab ${on ? "ttabs-tab-selected" : ""}`}
              onClick={() => onSelect(t.id)}
            >
              {t.label}
              {t.count !== undefined ? <span className="ttabs-count mono">{t.count}</span> : null}
            </button>
          );
        })}
      </div>
      <div
        className="ttabs-panel"
        role="tabpanel"
        id={`${base}-panel`}
        aria-labelledby={`${base}-tab-${selected}`}
      >
        {children}
      </div>
    </div>
  );
}
