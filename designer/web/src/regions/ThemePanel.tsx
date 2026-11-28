// SPDX-License-Identifier: MIT

// The Theme panel (#582, `view.theme`): a dock panel in the side bar group. It lists the project's theme tokens
// with one column per variant (light, dark, high contrast), a swatch and a text field per color, and the WCAG
// contrast result of every role pair under each variant. Editing a value is a source edit (`page-theme.ts`).
//
// Keyboard: Tab through the fields; Enter or leaving a field commits its value; Escape puts the stored value
// back. Pass or fail is written as text ("pass" / "fail"), never colour alone. Outcomes are announced through
// one polite live region.

import { useState, type KeyboardEvent, type ReactNode } from "react";
import { parseHex } from "../../theme-contrast.js";
import type { ThemeHandle } from "../page-theme";

function TokenField({ label, value, onCommit }: { label: string; value: string; onCommit: (value: string) => void }): ReactNode {
  const [draft, setDraft] = useState<string | null>(null);
  const shown = draft ?? value;
  const commit = (): void => {
    if (draft !== null && draft !== value) {
      onCommit(draft);
    }
    setDraft(null);
  };
  const onKeyDown = (event: KeyboardEvent<HTMLInputElement>): void => {
    if (event.key === "Enter") {
      event.preventDefault();
      commit();
    } else if (event.key === "Escape") {
      setDraft(null);
    }
  };
  const solid = parseHex(shown);
  return (
    <span className="theme-cell">
      {solid !== null && (
        <span className="theme-swatch" aria-hidden="true" style={{ background: shown }} />
      )}
      <input
        className="theme-input"
        type="text"
        spellCheck={false}
        aria-label={label}
        value={shown}
        onChange={(event) => setDraft(event.target.value)}
        onBlur={commit}
        onKeyDown={onKeyDown}
      />
    </span>
  );
}

export function ThemePanel({ theme, visible }: { theme: ThemeHandle; visible: boolean }): ReactNode {
  if (!visible) {
    return <section className="theme" id="theme-panel" hidden />;
  }
  if (theme.path === "") {
    return (
      <section className="theme" id="theme-panel" aria-label="Theme tokens">
        <p className="theme-status">No theme file in this project. A *.theme.json asset or a stylesheet with custom properties in :root shows here.</p>
      </section>
    );
  }
  const failing = theme.variants.reduce((sum, variant) => sum + variant.contrast.filter((row) => row.pass === false).length, 0);
  return (
    <section className="theme" id="theme-panel" aria-label="Theme tokens">
      <header className="theme-head">
        <select className="theme-file" aria-label="Token file" value={theme.path} onChange={(event) => theme.select(event.target.value)}>
          {theme.files.map((file) => (
            <option key={file} value={file}>{file}</option>
          ))}
        </select>
        <button type="button" className="theme-undo" disabled={!theme.canUndo} onClick={theme.undo}>Undo token edit</button>
      </header>
      <p className="theme-status" role="status" aria-live="polite">
        {theme.problem !== "" ? theme.problem : theme.notice !== "" ? theme.notice : failing === 0 ? "Every checked contrast pair passes." : failing + " contrast pair" + (failing === 1 ? "" : "s") + " fail."}
      </p>
      <div className="theme-scroll">
        <table className="theme-table" aria-label="Tokens by variant">
          <thead>
            <tr>
              <th scope="col">Token</th>
              {theme.variants.map((variant) => (
                <th scope="col" key={variant.id}>{variant.id}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {theme.rows.map((row) => (
              <tr key={row.name}>
                <th scope="row" className="theme-name">{row.name}</th>
                {theme.variants.map((variant, column) => {
                  const value = row.values[column];
                  return (
                    <td key={variant.id}>
                      {value === undefined ? (
                        <span className="theme-none">-</span>
                      ) : (
                        <TokenField label={row.name + " in " + variant.id} value={value} onCommit={(next) => theme.set(variant.id, row.name, next)} />
                      )}
                    </td>
                  );
                })}
              </tr>
            ))}
          </tbody>
        </table>
        <div className="theme-contrast">
          {theme.variants.map((variant) => (
            <section key={variant.id} aria-label={"Contrast, " + variant.id}>
              <h3 className="theme-h">{variant.id}</h3>
              <ul className="theme-pairs">
                {variant.contrast.map((pair) => (
                  <li key={pair.foreground + pair.background} className="theme-pair" data-pass={String(pair.pass)}>
                    <span className="theme-pair-name">{pair.foreground} on {pair.background}</span>
                    <span className="theme-pair-ratio">{pair.ratio === null ? "unchecked" : pair.ratio.toFixed(2) + ":1 (min " + pair.floor + ")"}</span>
                    <span className="theme-pair-verdict">{pair.pass === null ? "n/a" : pair.pass ? "pass" : "fail"}</span>
                  </li>
                ))}
              </ul>
            </section>
          ))}
        </div>
      </div>
    </section>
  );
}
