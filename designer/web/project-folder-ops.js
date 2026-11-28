// SPDX-License-Identifier: MIT

// Folder moves and folder deletes as pure plans (issue #391, directory operations of the folder IDE).
//
// A project is a flat list of files; a directory exists only as the prefix of a path (a zip carries no
// empty directory, and neither does this model). So "rename a folder" is "move every file under the
// prefix", and "delete a folder" is "delete every file under it". Both are decided here, as data, so the
// React workspace applies a plan in one state update and a Node gate can test the rules without a page.
//
// The rules are the file rules, applied to the whole set at once:
//   * only editable files move or go: a locked file (a font, C++, a generated header) is never written
//     or deleted by the studio (spec section 7), so a folder holding one is refused, naming the file;
//   * the target goes through `confinePath` (no `..`, no absolute path, no backslash);
//   * a folder cannot move into itself, onto a file, inside a file, or onto a path that is already taken;
//   * imports in other files are not rewritten (the UI says so, as for a file rename).
//
// No imports beyond project-paths.js, no DOM.

import { confinePath, isUnder } from './project-paths.js';

/**
 * @typedef {{ path: string, editable: boolean }} FolderOpFile
 * @typedef {{ ok: true, moves: { from: string, to: string }[] } | { ok: false, reason: string }} MovePlan
 */

/** The files of `files` strictly under the directory `dir`. */
export function filesUnder(files, dir) {
  const prefix = dir + '/';
  return files.filter((file) => file.path.startsWith(prefix));
}

function lockedUnder(inside) {
  const locked = inside.find((file) => !file.editable);
  return locked === undefined ? null : locked.path;
}

/**
 * Plans moving the directory `from` to `to`.
 * @param {readonly FolderOpFile[]} files
 * @returns {MovePlan}
 */
export function planFolderMove(files, from, to) {
  const inside = filesUnder(files, from);
  if (inside.length === 0) {
    return { ok: false, reason: `${from} is not a folder in this project` };
  }
  let target;
  try {
    target = confinePath(to);
  } catch (error) {
    return { ok: false, reason: error instanceof Error ? error.message : String(error) };
  }
  if (target === from) {
    return { ok: false, reason: `${from} is already there` };
  }
  if (isUnder(target, from)) {
    return { ok: false, reason: `${from} cannot move into itself` };
  }
  const locked = lockedUnder(inside);
  if (locked !== null) {
    return { ok: false, reason: `${locked} is locked, so the studio does not move folder ${from}` };
  }
  const moving = new Set(inside.map((file) => file.path));
  const others = files.filter((file) => !moving.has(file.path));
  if (others.some((file) => file.path === target)) {
    return { ok: false, reason: `${target} is a file in this project` };
  }
  if (others.some((file) => target.startsWith(file.path + '/'))) {
    return { ok: false, reason: `${target} would sit inside a file` };
  }
  const moves = inside.map((file) => ({ from: file.path, to: target + file.path.slice(from.length) }));
  const taken = new Set(others.map((file) => file.path));
  const clash = moves.find((move) => taken.has(move.to));
  if (clash !== undefined) {
    return { ok: false, reason: `${clash.to} already exists` };
  }
  return { ok: true, moves };
}

/**
 * Plans deleting the directory `dir`: the paths to remove, or why not.
 * @param {readonly FolderOpFile[]} files
 * @returns {{ ok: true, paths: string[] } | { ok: false, reason: string }}
 */
export function planFolderDelete(files, dir) {
  const inside = filesUnder(files, dir);
  if (inside.length === 0) {
    return { ok: false, reason: `${dir} is not a folder in this project` };
  }
  const locked = lockedUnder(inside);
  if (locked !== null) {
    return { ok: false, reason: `${locked} is locked, so the studio does not delete folder ${dir}` };
  }
  return { ok: true, paths: inside.map((file) => file.path) };
}
