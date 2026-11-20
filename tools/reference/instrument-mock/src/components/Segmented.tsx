// SPDX-License-Identifier: MIT

// A segmented selector: one choice from a few, as a recessed tray with the chosen segment raised and
// its LED lit (the reference's LOG/709 switch). Radio semantics: role=radiogroup, one Tab stop,
// arrows move and select (RovingFocus). `inverse` draws it for the dark band. Parts: `tseg`,
// `tseg-option` (`tseg-option-on`), `tseg-led`.
import { useRef, type KeyboardEvent } from "react";
import "./segmented.css";

export type SegmentedOption = { value: string; label: string; title?: string };

export type SegmentedProps = {
  label: string;
  value: string;
  options: SegmentedOption[];
  onChange: (value: string) => void;
  inverse?: boolean;
  vertical?: boolean;
};

export function Segmented({ label, value, options, onChange, inverse = false, vertical = false }: SegmentedProps) {
  const ref = useRef<HTMLDivElement>(null);
  const onKeyDown = (e: KeyboardEvent) => {
    const index = options.findIndex((o) => o.value === value);
    const last = options.length - 1;
    const forward = vertical ? "ArrowDown" : "ArrowRight";
    const back = vertical ? "ArrowUp" : "ArrowLeft";
    const next = e.key === forward ? (index === last ? 0 : index + 1) : e.key === back ? (index === 0 ? last : index - 1) : -1;
    if (next < 0) return;
    e.preventDefault();
    onChange(options[next].value);
    ref.current?.querySelectorAll<HTMLButtonElement>("[role=radio]")[next]?.focus();
  };
  return (
    <div
      ref={ref}
      className={`tseg ${inverse ? "tseg-inverse m-well" : "m-recess"} ${vertical ? "tseg-vertical" : ""}`}
      role="radiogroup"
      aria-label={label}
      onKeyDown={onKeyDown}
    >
      {options.map((o) => {
        const on = o.value === value;
        return (
          <button
            key={o.value}
            type="button"
            role="radio"
            aria-checked={on}
            title={o.title}
            tabIndex={on ? 0 : -1}
            className={`tseg-option ${on ? "tseg-option-on" : ""}`}
            onClick={() => onChange(o.value)}
          >
            <span className={`m-led ${on ? "m-led-on" : ""}`} aria-hidden="true" />
            {o.label}
          </button>
        );
      })}
    </div>
  );
}
