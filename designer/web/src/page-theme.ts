// SPDX-License-Identifier: MIT

// The page's side of the theme and design-token editor (#582, `view.theme`).
//
// **A token edit is a source edit.** The panel never keeps its own copy of the theme: it reads the token file's
// text from the workspace on every render, and a change replaces exactly one value span
// (`theme-tokens.js`). When the file is open in the editor the replacement is dispatched as one editor
// transaction, so the editor's Undo (Ctrl+Z), format, diff and the live apply all behave as for typing; a file
// the editor is not showing gets the buffer written and the page reloads the editor. The panel's own Undo button
// re-reads the spans and puts the previous value back, so it survives edits made elsewhere in the file.

import { useCallback, useMemo, useRef, useState } from "react";
import { checkVariant } from "../theme-contrast.js";
import { formatOf, isColorValue, readTokens, setToken } from "../theme-tokens.js";
import { applyEditorEdit } from "./editor/editor-target";
import type { Workspace } from "./page-workspace";

export interface ThemeVariant {
  id: string;
  tokens: ReadonlyMap<string, string>;
  contrast: ReturnType<typeof checkVariant>;
}

export interface ThemeRow {
  name: string;
  /** One value per variant column; undefined where the variant does not define the token. */
  values: (string | undefined)[];
  color: boolean;
}

export interface ThemeHandle {
  /** The files the editor can treat as token files (theme assets and stylesheets that define custom properties). */
  files: readonly string[];
  path: string;
  select(path: string): void;
  variants: readonly ThemeVariant[];
  rows: readonly ThemeRow[];
  /** A sentence when the file cannot be read as tokens, else empty. */
  problem: string;
  /** The outcome of the last edit or refusal, for the live region. */
  notice: string;
  canUndo: boolean;
  set(variantId: string, name: string, value: string): void;
  undo(): void;
}

export interface ThemeDeps {
  workspace: Workspace;
  /** Called with the path when a write went to the buffer instead of the editor, so the page reloads the editor. */
  onWritten: (paths: readonly string[]) => void;
  visible: boolean;
}

interface Change {
  path: string;
  variantId: string;
  name: string;
  previous: string;
}

export function useTheme(deps: ThemeDeps): ThemeHandle {
  const depsRef = useRef(deps);
  depsRef.current = deps;
  const { workspace } = deps;
  const [chosen, setChosen] = useState("");
  const [notice, setNotice] = useState("");
  const [stack, setStack] = useState<readonly Change[]>([]);

  const files = workspace.state.files;
  const dirty = workspace.state.dirty;
  const candidates = useMemo(() => {
    if (!deps.visible) {
      return [];
    }
    return files
      .filter((file) => file.editable && formatOf(file.path) !== null)
      .filter((file) => {
        const format = formatOf(file.path) as "json" | "css";
        const read = readTokens(workspace.textOf(file.path), format);
        return !read.ok || read.variants.some((variant: { tokens: unknown[] }) => variant.tokens.length > 0);
      })
      .map((file) => file.path);
    // dirty is a dependency: an edit changes the text without changing the file list.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [deps.visible, files, dirty]);

  const path = candidates.includes(chosen) ? chosen : (candidates.find((candidate) => formatOf(candidate) === "json") ?? candidates[0] ?? "");
  const format = path === "" ? null : (formatOf(path) as "json" | "css");
  const text = deps.visible && path !== "" ? workspace.textOf(path) : "";

  const model = useMemo(() => {
    if (format === null) {
      return { problem: "", variants: [] as ThemeVariant[], rows: [] as ThemeRow[] };
    }
    const read = readTokens(text, format);
    if (!read.ok) {
      return { problem: read.message as string, variants: [] as ThemeVariant[], rows: [] as ThemeRow[] };
    }
    const variants: ThemeVariant[] = read.variants.map((variant: { id: string; tokens: { name: string; value: string }[] }) => {
      const tokens = new Map(variant.tokens.map((token) => [token.name, token.value] as const));
      return { id: variant.id, tokens, contrast: checkVariant(Object.fromEntries(tokens), variant.id) };
    });
    const names: string[] = [];
    for (const variant of variants) {
      for (const name of variant.tokens.keys()) {
        if (!names.includes(name)) {
          names.push(name);
        }
      }
    }
    const rows = names.map((name) => {
      const values = variants.map((variant) => variant.tokens.get(name));
      return { name, values, color: values.some((value) => value !== undefined && isColorValue(value)) };
    });
    return { problem: "", variants, rows };
  }, [text, format]);

  /** Replaces one value span: through the editor when it shows the file (so Ctrl+Z works), else the buffer. */
  const write = useCallback((target: string, variantId: string, name: string, value: string): { ok: boolean; message: string; previous: string } => {
    const { workspace: space } = depsRef.current;
    const kind = formatOf(target) as "json" | "css";
    const before = space.textOf(target);
    const result = setToken(before, kind, variantId, name, value);
    if (!result.ok) {
      return { ok: false, message: result.message as string, previous: "" };
    }
    if (result.text === before) {
      return { ok: true, message: "", previous: result.previous as string };
    }
    let start = 0;
    while (start < before.length && before[start] === result.text[start]) {
      start += 1;
    }
    let endBefore = before.length;
    let endAfter = result.text.length;
    while (endBefore > start && endAfter > start && before[endBefore - 1] === result.text[endAfter - 1]) {
      endBefore -= 1;
      endAfter -= 1;
    }
    if (!applyEditorEdit(target, { from: start, to: endBefore, insert: result.text.slice(start, endAfter) })) {
      space.writeBuffer(target, result.text);
      depsRef.current.onWritten([target]);
    }
    return { ok: true, message: "", previous: result.previous as string };
  }, []);

  const set = useCallback(
    (variantId: string, name: string, value: string): void => {
      if (path === "") {
        return;
      }
      const outcome = write(path, variantId, name, value);
      if (!outcome.ok) {
        setNotice(outcome.message);
        return;
      }
      if (outcome.previous !== value.trim()) {
        setStack((previous) => [...previous, { path, variantId, name, previous: outcome.previous }]);
        setNotice(name + " in " + variantId + " set to " + value.trim() + ".");
      }
    },
    [path, write],
  );

  const undo = useCallback((): void => {
    const last = stack[stack.length - 1];
    if (last === undefined) {
      return;
    }
    const outcome = write(last.path, last.variantId, last.name, last.previous);
    setStack(stack.slice(0, -1));
    setNotice(outcome.ok ? last.name + " in " + last.variantId + " restored to " + last.previous + "." : outcome.message);
  }, [stack, write]);

  return {
    files: candidates,
    path,
    select: setChosen,
    variants: model.variants,
    rows: model.rows,
    problem: model.problem,
    notice,
    canUndo: stack.length > 0,
    set,
    undo,
  };
}
