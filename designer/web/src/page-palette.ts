// SPDX-License-Identifier: MIT

// The page's side of the command palette, quick open and go to line (issue #558): the open/close state, the
// persisted recent lists, and the few facts the overlay needs from the page (the files, how many lines the
// editor has, how to open a file and move the caret). The ranking, the input modes and the argument flow are
// `workbench/palette.js`; the overlay is `regions/CommandPalette.tsx`.

import { useCallback, useRef, useState, type MutableRefObject } from "react";
import { createRecentList, type MenuProviders, type RecentList } from "./workbench/index.js";
import { editorCaret, editorGotoLine, editorLineCount } from "./editor/editor-target";
import type { LoadedSample } from "./lib/gallery";
import type { WorkbenchApi } from "./page-commands";

const RECENT_COMMANDS_KEY = "tessera.studio.recentCommands";
const RECENT_FILES_KEY = "tessera.studio.recentFiles";

/** One request to show the overlay; `nonce` remounts it so a second request starts from a clean input. */
export interface PaletteRequest {
  initial: string;
  intent: "commands" | "files" | "line" | "shortcuts" | "samples";
  nonce: number;
}

export interface PaletteHost {
  request: PaletteRequest | null;
  open: (initial: string, intent?: PaletteRequest["intent"]) => void;
  close: () => void;
  files: () => readonly { path: string }[];
  /** A chosen sample (its source already fetched) becomes the open project; the Open Sample quick pick calls it. */
  pickSample: (sample: LoadedSample) => void;
  openFile: (path: string, line?: number) => void;
  providers: () => MenuProviders;
  recentCommands: RecentList;
  recentFiles: RecentList;
  /** Persists both recent lists (storage may be absent or full; that is silently fine). */
  saveRecent: () => void;
  lineCount: () => number;
  caretLine: () => number | null;
  gotoLine: (line: number) => void;
}

function readList(key: string, limit: number): RecentList {
  const list = createRecentList(limit);
  try {
    const raw = globalThis.localStorage?.getItem(key);
    if (raw) {
      list.load(JSON.parse(raw));
    }
  } catch {
    // Storage may be blocked or hold something else; an empty list is the right fallback.
  }
  return list;
}

export function usePaletteHost(
  apiRef: MutableRefObject<WorkbenchApi>,
  providersRef: MutableRefObject<MenuProviders>,
): PaletteHost {
  const [request, setRequest] = useState<PaletteRequest | null>(null);
  const nonce = useRef(0);
  const lists = useRef<{ commands: RecentList; files: RecentList } | null>(null);
  if (lists.current === null) {
    lists.current = { commands: readList(RECENT_COMMANDS_KEY, 8), files: readList(RECENT_FILES_KEY, 12) };
  }
  const { commands, files } = lists.current;

  const open = useCallback((initial: string, intent: PaletteRequest["intent"] = "commands") => {
    nonce.current += 1;
    setRequest({ initial, intent, nonce: nonce.current });
  }, []);
  const close = useCallback(() => setRequest(null), []);

  return {
    request,
    open,
    close,
    files: () => apiRef.current.files,
    pickSample: (sample) => apiRef.current.pickSample(sample),
    openFile: (path, line) => {
      apiRef.current.openFile(path);
      files.touch(path);
      // The tab switch is a state change; the editor shows the file one render later, so the caret moves then.
      window.setTimeout(() => {
        // Someone who already reopened the palette (or went elsewhere) must not have focus pulled away.
        if (document.getElementById("palette-input") !== null) {
          return;
        }
        if (line !== undefined) {
          editorGotoLine(line);
        } else {
          document.getElementById("source")?.focus();
        }
      }, 60);
    },
    providers: () => providersRef.current,
    recentCommands: commands,
    recentFiles: files,
    saveRecent: () => {
      try {
        globalThis.localStorage?.setItem(RECENT_COMMANDS_KEY, JSON.stringify(commands));
        globalThis.localStorage?.setItem(RECENT_FILES_KEY, JSON.stringify(files));
      } catch {
        // Not persisting the recent lists never breaks the palette.
      }
    },
    lineCount: editorLineCount,
    caretLine: () => editorCaret()?.line ?? null,
    gotoLine: editorGotoLine,
  };
}
