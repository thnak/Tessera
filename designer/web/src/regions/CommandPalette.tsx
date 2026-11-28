// SPDX-License-Identifier: MIT

// The command palette, Quick Open and Go to Line overlay (issue #558): one input, three modes chosen by its
// prefix (`>` commands, `:` line, otherwise files), and the argument prompts of a command with a JSON Schema
// `args`. Ranking, modes and the prompt flow are `workbench/palette.js`; this draws the result.
//
// ARIA: the input is a `combobox` controlling a `listbox` of `option`s and names the highlighted one with
// `aria-activedescendant`, so focus stays in the input while the arrow keys move through the list. Escape
// closes and returns focus to where it was; blur closes too, like VS Code's quick input.

import { useEffect, useMemo, useRef, useState, type KeyboardEvent } from "react";
import {
  commandItems,
  createArgFlow,
  needsPrompt,
  parseInput,
  parseLine,
  rankItems,
  type ArgFlow,
  type ArgChoice,
  type CommandArgs,
  type RankItem,
  type Ranked,
} from "../workbench/index.js";
import { referencePick } from "../lib/reference-pick";
import type { Workbench } from "../page-commands";
import type { PaletteRequest } from "../page-palette";

/** The label with its matched characters in `<b>`. */
function Highlighted({ text, positions }: { text: string; positions: readonly number[] }) {
  if (positions.length === 0) {
    return <>{text}</>;
  }
  const marked = new Set(positions);
  const runs: { text: string; hit: boolean }[] = [];
  for (let i = 0; i < text.length; i += 1) {
    const hit = marked.has(i);
    const last = runs.at(-1);
    if (last !== undefined && last.hit === hit) {
      last.text += text[i];
    } else {
      runs.push({ text: text[i], hit });
    }
  }
  return (
    <>
      {runs.map((run, index) => (run.hit ? <b key={index}>{run.text}</b> : <span key={index}>{run.text}</span>))}
    </>
  );
}

interface Row {
  id: string;
  label: string;
  detail?: string;
  chord?: string;
  positions: readonly number[];
  commit: () => void;
}

const PLACEHOLDERS: Record<PaletteRequest["intent"], string> = {
  commands: "Type the name of a command to run",
  files: "Search files by name (add :line to jump, > for commands)",
  line: "Type a line number",
  shortcuts: "Keyboard shortcuts: type to filter every command and its chord",
  samples: "Choose a sample of the opened folder (type to filter, > for commands)",
  references: "References of the name at the caret (type to filter)",
};

export function CommandPalette({ workbench, request }: { workbench: Workbench; request: PaletteRequest }) {
  const { palette, registry } = workbench;
  const [value, setValue] = useState(request.initial);
  const [active, setActive] = useState(0);
  const [flow, setFlow] = useState<ArgFlow | null>(null);
  const [message, setMessage] = useState("");
  const input = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLUListElement>(null);
  const returnTo = useRef<Element | null>(null);
  const closed = useRef(false);

  useEffect(() => {
    returnTo.current = document.activeElement;
    workbench.setFocusOwner("palette");
    input.current?.focus();
    input.current?.setSelectionRange(request.initial.length, request.initial.length);
    return () => workbench.setFocusOwner(null);
    // Once per mount: the key (nonce) remounts for a new request.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  function close(restore = true) {
    if (closed.current) {
      return;
    }
    closed.current = true;
    palette.saveRecent();
    palette.close();
    if (restore && returnTo.current instanceof HTMLElement && document.contains(returnTo.current)) {
      returnTo.current.focus();
    }
  }

  function execute(id: string, args?: CommandArgs) {
    palette.recentCommands.touch(id);
    close();
    void workbench.run(id, args);
  }

  function chooseCommand(id: string) {
    const record = registry.get(id);
    if (record === undefined) {
      return;
    }
    if (needsPrompt(record, {})) {
      const next = createArgFlow(record, {}, palette.providers());
      setFlow(next);
      setValue("");
      setActive(0);
      setMessage("");
    } else {
      execute(id);
    }
  }

  function answer(next: ArgFlow) {
    if (next.done) {
      execute(next.record.id, next.args);
    } else {
      setFlow(next);
      setValue("");
      setActive(0);
      setMessage("");
    }
  }

  const parsed = parseInput(value);
  const rows: Row[] = useMemo(() => {
    if (flow !== null) {
      const step = flow.step;
      if (step === null || step.kind !== "choice" || step.choices === undefined) {
        return [];
      }
      const items = step.choices.map((choice: ArgChoice, index) => ({ id: String(index), label: choice.label, detail: choice.detail }));
      // A choice list keeps the schema's own order until the person types (presets are not alphabetical).
      const ranked: Ranked[] = value.trim() === "" ? items.map((item) => ({ item, score: 0, positions: [], basenameMatch: false })) : rankItems(value.trim(), items);
      return ranked.map(({ item, positions }: Ranked): Row => ({
        id: `choice-${item.id}`,
        label: item.label,
        detail: item.detail,
        positions,
        commit: () => answer(flow.answer((step.choices as ArgChoice[])[Number(item.id)].value)),
      }));
    }
    if (parsed.mode === "commands") {
      return rankItems(parsed.text, commandItems(registry), { recent: palette.recentCommands }).map(({ item, positions }: Ranked<RankItem>): Row => ({
        id: `cmd-${item.id}`,
        label: item.label,
        detail: item.detail,
        chord: item.chord,
        positions,
        commit: () => chooseCommand(item.id),
      }));
    }
    if (parsed.mode === "files" && request.intent === "references") {
      const items = referencePick.rows().map((row, index) => ({ id: String(index), label: `${row.path}:${row.line}:${row.column}`, detail: row.preview }));
      return rankItems(parsed.text, items).map(({ item, positions }: Ranked<RankItem>): Row => ({
        id: `reference-${item.id}`,
        label: item.label,
        detail: item.detail,
        positions,
        commit: () => {
          close(false);
          referencePick.choose(Number(item.id));
        },
      }));
    }
    if (parsed.mode === "files" && request.intent === "samples") {
      // The entries of the folder the person picked from their clone; the page bundles no sample list.
      const items = palette.sampleChoices().map((path) => ({ id: path, label: path.split("/").at(-1) ?? path, detail: path }));
      return rankItems(parsed.text, items).map(({ item, positions }: Ranked<RankItem>): Row => ({
        id: `sample-${item.id}`,
        label: item.label,
        detail: item.detail,
        positions,
        commit: () => {
          close(false);
          palette.pickSample(item.id);
        },
      }));
    }
    if (parsed.mode === "files") {
      const items = palette.files().map((file) => ({ id: file.path, label: file.path, basename: file.path.split("/").at(-1) }));
      return rankItems(parsed.text, items, { recent: palette.recentFiles }).map(({ item, positions }: Ranked<RankItem>): Row => ({
        id: `file-${item.id}`,
        label: item.label,
        positions,
        commit: () => {
          close(false);
          palette.openFile(item.id, parsed.line);
        },
      }));
    }
    const target = parseLine(parsed.text, palette.lineCount());
    return target.ok
      ? [
          {
            id: "line",
            label: `Go to line ${target.line}`,
            positions: [],
            commit: () => {
              close(false);
              palette.gotoLine(target.line);
            },
          },
        ]
      : [];
    // `answer`, `chooseCommand` and `close` read the latest state through the closure of this render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [value, flow, registry, workbench.version()]);

  const step = flow?.step ?? null;
  const textStep = step !== null && step.kind !== "choice";
  const limit = Math.max(rows.length, 1);
  const activeIndex = Math.min(active, limit - 1);
  const lineHint = flow === null && parsed.mode === "line" ? parseLine(parsed.text, palette.lineCount()) : null;
  const status = textStep ? message || `${step.label}${step.hint ? ` (${step.hint})` : ""}: type it and press Enter` : lineHint !== null && !lineHint.ok ? lineHint.message : "";

  useEffect(() => {
    listRef.current?.querySelector<HTMLElement>('[aria-selected="true"]')?.scrollIntoView({ block: "nearest" });
  }, [activeIndex, rows]);

  function onKeyDown(event: KeyboardEvent<HTMLInputElement>) {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      close();
    } else if (event.key === "ArrowDown") {
      event.preventDefault();
      setActive((activeIndex + 1) % limit);
    } else if (event.key === "ArrowUp") {
      event.preventDefault();
      setActive((activeIndex - 1 + limit) % limit);
    } else if (event.key === "PageDown") {
      event.preventDefault();
      setActive(Math.min(activeIndex + 8, limit - 1));
    } else if (event.key === "PageUp") {
      event.preventDefault();
      setActive(Math.max(activeIndex - 8, 0));
    } else if (event.key === "Enter") {
      event.preventDefault();
      if (textStep && flow !== null) {
        const result = flow.answerText(value);
        if (result.ok) {
          answer(result.flow);
        } else {
          setMessage(result.message);
        }
      } else {
        rows[activeIndex]?.commit();
      }
    } else if (event.key === "Backspace" && value === "" && flow !== null) {
      event.preventDefault();
      if (flow.index === 0) {
        setFlow(null);
        setValue(">");
      } else {
        setFlow(flow.back());
      }
    }
  }

  const title = flow !== null ? `${flow.record.category}: ${flow.record.title}` : "";
  const placeholder = textStep
    ? `${step.label}${step.hint ? ` (${step.hint})` : ""}`
    : step !== null
      ? `Choose ${step.label.toLowerCase()}`
      : PLACEHOLDERS[request.intent];
  const listboxId = "palette-list";

  return (
    <div className="palette-layer">
      <div className="palette" role="dialog" aria-label={title === "" ? "Command palette" : title} aria-modal="false" onBlur={(event) => {
        if (!event.currentTarget.contains(event.relatedTarget as Node | null)) {
          close(false);
        }
      }}>
        {title === "" ? null : <div className="palette-title">{title}</div>}
        <input
          ref={input}
          id="palette-input"
          className="palette-input"
          type="text"
          role="combobox"
          aria-expanded={rows.length > 0}
          aria-controls={listboxId}
          aria-autocomplete="list"
          aria-activedescendant={rows.length > 0 ? `palette-opt-${activeIndex}` : undefined}
          aria-label={placeholder}
          aria-invalid={message !== "" ? true : undefined}
          aria-describedby="palette-status"
          placeholder={placeholder}
          autoComplete="off"
          spellCheck={false}
          value={value}
          onChange={(event) => {
            setValue(event.currentTarget.value);
            setActive(0);
            setMessage("");
          }}
          onKeyDown={onKeyDown}
        />
        <ul className="palette-list" role="listbox" id={listboxId} aria-label="Results" ref={listRef} onMouseDown={(event) => event.preventDefault()} hidden={rows.length === 0}>
          {rows.map((row, index) => (
            <li
              key={row.id}
              id={`palette-opt-${index}`}
              role="option"
              aria-selected={index === activeIndex}
              className={`palette-option${index === activeIndex ? " is-active" : ""}`}
              onMouseMove={() => setActive(index)}
              onClick={() => row.commit()}
            >
              <span className="po-label">
                <Highlighted text={row.label} positions={row.positions} />
              </span>
              {row.detail ? <span className="po-detail">{row.detail}</span> : null}
              {row.chord ? <kbd className="po-chord">{row.chord}</kbd> : null}
            </li>
          ))}
        </ul>
        <div className="palette-status" id="palette-status" role="status" aria-live="polite">
          {status !== "" ? status : rows.length === 0 && flow === null && parsed.mode !== "line" ? "No matching results" : `${rows.length} result${rows.length === 1 ? "" : "s"}`}
        </div>
      </div>
    </div>
  );
}
