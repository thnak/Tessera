// SPDX-License-Identifier: MIT

// The Search panel (#580, `view.search`, Ctrl+Shift+F): a dock panel in the side bar group next to the project
// tree. A query field with its three toggles (match case, whole word, regular expression), an optional replace
// field, include and exclude globs, and the results grouped by file; a result opens its file at the match.
// The searching runs in a worker (`page-search.ts`); this file only draws the handle it is given.
//
// Keyboard: Tab through the fields; Enter in the query searches now; Alt+C, Alt+W and Alt+R flip the toggles
// from anywhere in the panel; Arrow keys, Home and End move between results and Enter opens one; Ctrl+Alt+Enter
// replaces all; Ctrl+Z and Ctrl+Shift+Z in the results undo and redo the last replace (a text field keeps the
// browser's own undo). Counts and outcomes are announced through one polite live region.

import { useEffect, useRef, type KeyboardEvent, type ReactNode } from "react";
import type { SearchMatch } from "../lib/search-client";
import { summaryOf, type SearchHandle } from "../page-search";

const TOGGLES = [
  { key: "caseSensitive", label: "Match Case", hint: "Alt+C", glyph: "Aa", chord: "c" },
  { key: "wholeWord", label: "Match Whole Word", hint: "Alt+W", glyph: "ab", chord: "w" },
  { key: "regex", label: "Use Regular Expression", hint: "Alt+R", glyph: ".*", chord: "r" },
] as const;

function moveFocus(event: KeyboardEvent<HTMLElement>): void {
  const key = event.key;
  if (key !== "ArrowDown" && key !== "ArrowUp" && key !== "Home" && key !== "End") {
    return;
  }
  const rows = Array.from(event.currentTarget.querySelectorAll<HTMLElement>("[data-search-row], [data-search-file]"));
  const index = rows.indexOf(document.activeElement as HTMLElement);
  if (rows.length === 0) {
    return;
  }
  event.preventDefault();
  const next = key === "Home" ? 0 : key === "End" ? rows.length - 1 : Math.min(Math.max(index + (key === "ArrowDown" ? 1 : -1), 0), rows.length - 1);
  rows[next].focus();
}

function Preview({ match }: { match: SearchMatch }): ReactNode {
  return (
    <span className="search-text">
      {match.text.slice(0, match.start)}
      <mark>{match.text.slice(match.start, match.end)}</mark>
      {match.text.slice(match.end)}
    </span>
  );
}

/** The line under the fields: a pattern problem, else the last replace outcome, else the result summary. */
function statusText(search: SearchHandle): string {
  if (search.message !== "") {
    return search.message;
  }
  if (search.notice !== "") {
    return search.notice;
  }
  return search.result !== null ? summaryOf(search.result) : "Type to search the open project. Locked files are not searched.";
}

export function SearchPanel({ search, visible }: { search: SearchHandle; visible: boolean }) {
  const panel = useRef<HTMLElement | null>(null);
  const held = useRef(false);
  // A new result list replaces the rows, and a focused row that is removed drops the focus to the page. Keep it
  // in the panel so the next key (another Ctrl+Z, an arrow) still reaches it.
  useEffect(() => {
    if (held.current && document.activeElement === document.body) {
      panel.current?.focus();
    }
  }, [search.answered]);
  if (!visible) {
    return <section className="search" id="search-panel" hidden />;
  }
  const { query, result } = search;
  const canReplaceAll = result !== null && result.total > 0 && result.truncated === null;

  const onPanelKey = (event: KeyboardEvent<HTMLElement>): void => {
    if (event.altKey && !event.ctrlKey && !event.metaKey) {
      const toggle = TOGGLES.find((item) => item.chord === event.key.toLowerCase());
      if (toggle !== undefined) {
        event.preventDefault();
        search.setQuery({ [toggle.key]: !query[toggle.key] });
        return;
      }
    }
    if ((event.ctrlKey || event.metaKey) && event.altKey && event.key === "Enter") {
      event.preventDefault();
      search.replaceAll();
      return;
    }
    const target = event.target as HTMLElement;
    const inField = target.tagName === "INPUT" || target.tagName === "TEXTAREA";
    if (!inField && (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "z") {
      event.preventDefault();
      if (event.shiftKey) {
        search.redo();
      } else {
        search.undo();
      }
    }
  };

  return (
    <section
      className="search"
      id="search-panel"
      aria-label="Search"
      tabIndex={-1}
      ref={panel}
      onKeyDown={onPanelKey}
      onFocus={() => {
        held.current = true;
      }}
      onBlur={(event) => {
        if (event.relatedTarget !== null) {
          held.current = false;
        }
      }}
    >
      <form
        className="search-form"
        role="search"
        onSubmit={(event) => {
          event.preventDefault();
          search.refresh();
        }}
      >
        <div className="search-row">
          <button
            type="button"
            className="search-disclosure"
            aria-expanded={search.showReplace}
            aria-controls="search-replace"
            aria-label="Toggle Replace"
            title="Toggle Replace"
            onClick={() => search.setShowReplace(!search.showReplace)}
          >
            <span aria-hidden="true">{search.showReplace ? "▾" : "▸"}</span>
          </button>
          <input
            id="search-query"
            className="search-input"
            type="text"
            aria-label="Search"
            placeholder="Search"
            autoComplete="off"
            spellCheck={false}
            value={query.query}
            aria-invalid={search.status === "error"}
            aria-describedby="search-status"
            onChange={(event) => search.setQuery({ query: event.target.value })}
          />
          {TOGGLES.map((toggle) => (
            <button
              key={toggle.key}
              type="button"
              className="search-toggle"
              aria-pressed={query[toggle.key]}
              aria-label={toggle.label}
              title={`${toggle.label} (${toggle.hint})`}
              data-search-toggle={toggle.key}
              onClick={() => search.setQuery({ [toggle.key]: !query[toggle.key] })}
            >
              <span aria-hidden="true">{toggle.glyph}</span>
            </button>
          ))}
        </div>
        {search.showReplace ? (
          <div className="search-row search-replace-row" id="search-replace">
            <input
              id="search-replace-input"
              className="search-input"
              type="text"
              aria-label="Replace"
              placeholder="Replace"
              autoComplete="off"
              spellCheck={false}
              value={search.replacement}
              onChange={(event) => search.setReplacement(event.target.value)}
            />
            <button
              type="button"
              className="search-action"
              id="search-replace-all"
              disabled={!canReplaceAll}
              title={canReplaceAll ? "Replace All (Ctrl+Alt+Enter)" : "Replace All needs a complete result list"}
              onClick={() => search.replaceAll()}
            >
              Replace All
            </button>
          </div>
        ) : null}
        <div className="search-row">
          <button
            type="button"
            className="search-disclosure search-filters-toggle"
            aria-expanded={search.showFilters}
            aria-controls="search-filters"
            aria-label="Toggle Search Details"
            title="Toggle Search Details"
            onClick={() => search.setShowFilters(!search.showFilters)}
          >
            <span aria-hidden="true">{"…"}</span>
          </button>
          <div className="search-undo" role="group" aria-label="Replace history">
            <button type="button" className="search-action" id="search-undo" disabled={!search.canUndo} title={search.canUndo ? `Undo ${search.undoLabel}` : "Nothing to undo"} onClick={() => search.undo()}>
              Undo Replace
            </button>
            <button type="button" className="search-action" id="search-redo" disabled={!search.canRedo} title={search.canRedo ? `Redo ${search.redoLabel}` : "Nothing to redo"} onClick={() => search.redo()}>
              Redo
            </button>
          </div>
        </div>
        {search.showFilters ? (
          <div className="search-filters" id="search-filters">
            <label>
              <span>Files to include</span>
              <input id="search-include" className="search-input" type="text" placeholder="e.g. *.tsx, ui/**" autoComplete="off" spellCheck={false} value={query.include} onChange={(event) => search.setQuery({ include: event.target.value })} />
            </label>
            <label>
              <span>Files to exclude</span>
              <input id="search-exclude" className="search-input" type="text" placeholder="e.g. dist, **/*.test.tsx" autoComplete="off" spellCheck={false} value={query.exclude} onChange={(event) => search.setQuery({ exclude: event.target.value })} />
            </label>
          </div>
        ) : null}
      </form>
      <p className={`search-status${search.status === "error" ? " search-error" : ""}`} id="search-status" role="status" aria-live="polite" data-answered={search.answered}>
        {statusText(search)}
      </p>
      {result !== null && result.files.length > 0 ? (
        <ul className="search-results" id="search-results" aria-label="Search results" onKeyDown={moveFocus}>
          {result.files.map((group, groupIndex) => {
            const folded = search.collapsed.has(group.path);
            return (
              <li key={group.path} className="search-group" data-search-path={group.path}>
                <div className="search-file-row">
                  <button
                    type="button"
                    className="search-file"
                    id={`search-file-${groupIndex}`}
                    data-search-file={group.path}
                    aria-expanded={!folded}
                    onClick={() => search.toggleFile(group.path)}
                  >
                    <span aria-hidden="true">{folded ? "▸" : "▾"}</span> <span className="search-file-name">{group.path}</span>{" "}
                    <span className="search-count" role="img" aria-label={`${group.matches.length} ${group.matches.length === 1 ? "match" : "matches"}`}>
                      {group.matches.length}
                    </span>
                  </button>
                  {search.showReplace ? (
                    <button type="button" className="search-action search-replace-file" aria-label={`Replace all in ${group.path}`} title="Replace all in this file" onClick={() => search.replaceFile(group.path)}>
                      Replace
                    </button>
                  ) : null}
                </div>
                {folded ? null : (
                  <ul className="search-matches" aria-labelledby={`search-file-${groupIndex}`}>
                    {group.matches.map((match) => (
                      <li key={match.offset} className="search-match-row">
                        <button
                          type="button"
                          className="search-row-button"
                          data-search-row={`${group.path}:${match.offset}`}
                          data-line={match.line}
                          data-column={match.column}
                          aria-label={`Line ${match.line}, column ${match.column}: ${match.text}`}
                          onClick={() => search.open(group.path, match)}
                        >
                          <span className="search-where" aria-hidden="true">{match.line}</span>
                          <Preview match={match} />
                        </button>
                        {search.showReplace ? (
                          <button type="button" className="search-action search-replace-one" aria-label={`Replace this match on line ${match.line} of ${group.path}`} title="Replace" onClick={() => search.replaceMatch(group.path, match)}>
                            Replace
                          </button>
                        ) : null}
                      </li>
                    ))}
                  </ul>
                )}
              </li>
            );
          })}
        </ul>
      ) : null}
    </section>
  );
}
