// SPDX-License-Identifier: MIT

// The list of recently opened project folders the project bar offers (issue #391, owner direction
// 2026-11-17). The storage and the list policy are `recent-folder.js`; this hook only mirrors the stored
// list into React state and keeps the handles, which are capabilities and never rendered, beside it.

import { useCallback, useEffect, useRef, useState } from "react";
import { forgetAllFolders, forgetFolder, recallFolders, rememberFolder } from "../recent-folder.js";

/** One remembered folder, as the project control lists it. */
export interface RecentProject {
  name: string;
  /** Epoch ms of the last open; 0 for a folder remembered before times were kept. */
  openedAt: number;
}

export interface RecentProjects {
  /** Newest first, at most ten. */
  recents: readonly RecentProject[];
  /** The stored handle of `recents[index]`, or `undefined`. */
  handleAt: (index: number) => FileSystemDirectoryHandle | undefined;
  /** Remembers `handle` as the most recently opened folder. */
  remember: (handle: FileSystemDirectoryHandle) => void;
  /** Drops one folder that no longer opens. */
  forget: (handle: FileSystemDirectoryHandle) => void;
  /** Clears the list; the folders themselves are not touched. */
  forgetAll: () => void;
}

export function useRecentProjects(): RecentProjects {
  const [recents, setRecents] = useState<readonly RecentProject[]>([]);
  const handles = useRef<FileSystemDirectoryHandle[]>([]);
  const refresh = useCallback(async () => {
    const list: { name: string; openedAt: number; handle: FileSystemDirectoryHandle }[] = await recallFolders();
    handles.current = list.map((entry) => entry.handle);
    setRecents(list.map((entry) => ({ name: entry.name, openedAt: entry.openedAt })));
  }, []);
  useEffect(() => {
    void refresh();
  }, [refresh]);
  const handleAt = useCallback((index: number) => handles.current[index], []);
  const remember = useCallback((handle: FileSystemDirectoryHandle) => void rememberFolder(handle).then(refresh), [refresh]);
  const forget = useCallback((handle: FileSystemDirectoryHandle) => void forgetFolder(handle).then(refresh), [refresh]);
  const forgetAll = useCallback(() => void forgetAllFolders().then(refresh), [refresh]);
  return { recents, handleAt, remember, forget, forgetAll };
}
