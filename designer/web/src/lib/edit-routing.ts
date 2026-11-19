// SPDX-License-Identifier: MIT

// Where `edit.undo` / `edit.redo` go (issue #585, owner-accepted rule D3).
//
// **One Undo only ever touches one history, chosen by where the user is working. There is no merged
// timeline in V1.** Two histories exist: the override stack the canvas module keeps (designer-protocol-v1
// section 9.4) and the text history of the code editor (CodeMirror). A third owner, the inspector's text
// inputs, keeps the browser's native undo until they blur.
//
// The rules are a pure function of the focus and of which domain was acted on last, so the Node gate checks
// the whole routing table and a Tessera-built studio can reuse it. No DOM, no React, no imports.
//
//   focus                         target
//   code editor                   text      (CodeMirror history)
//   canvas, inspector controls    override  (the module's stack)
//   a text input (the inspector's value fields, a rename or palette field)
//                                 native    (the browser's, until blur; the chord is left alone)
//   anything else                 the domain acted on last (menu bar, palette, dock chrome)

/** The element class the focus is in. */
export type EditFocus = "editor" | "canvas" | "inspector" | "text-input" | "other";

/** The history a menu item or a chord at a given focus acts on. */
export type EditTarget = "override" | "text" | "native";

/** A history stack's facts, as the menu needs them. */
export interface StackFacts {
  canUndo: boolean;
  canRedo: boolean;
  /** What Undo would undo, e.g. `Set color on Button`; may be `""`. */
  undoLabel: string;
  redoLabel: string;
}

export const NO_STACK: StackFacts = Object.freeze({ canUndo: false, canRedo: false, undoLabel: "", redoLabel: "" });

/** The domain a focus names, or `null` when it names none (so "last acted" decides). */
function domainOf(focus: EditFocus): "override" | "text" | null {
  switch (focus) {
    case "editor":
      return "text";
    case "canvas":
    case "inspector":
      return "override";
    default:
      return null;
  }
}

/**
 * The history an Undo or Redo at `focus` acts on. `last` is the domain the user acted on most recently
 * (focus entered it, or a command ran in it); with nothing acted on yet the text history is the default.
 */
export function targetFor(focus: EditFocus, last: "override" | "text" | null): EditTarget {
  if (focus === "text-input") {
    return "native";
  }
  return domainOf(focus) ?? last ?? "text";
}

/** What the Edit menu and the context keys show for a target. */
export interface EditMenuState {
  target: EditTarget;
  canUndo: boolean;
  canRedo: boolean;
  /** `Undo Set color on Button`, `Undo typing`, or plain `Undo`. */
  undoTitle: string;
  redoTitle: string;
}

/**
 * The menu's view of a target. An empty stack disables its item (the menu keeps it focusable and sets
 * `aria-disabled`); the native target is never disabled, because the browser owns that stack.
 */
export function menuState(
  target: EditTarget,
  stacks: { override: StackFacts; text: StackFacts },
): EditMenuState {
  if (target === "native") {
    return { target, canUndo: true, canRedo: true, undoTitle: "Undo", redoTitle: "Redo" };
  }
  const facts = stacks[target];
  const word = (verb: string, label: string): string => (label === "" ? verb : `${verb} ${label}`);
  return {
    target,
    canUndo: facts.canUndo,
    canRedo: facts.canRedo,
    undoTitle: word("Undo", facts.canUndo ? facts.undoLabel : ""),
    redoTitle: word("Redo", facts.canRedo ? facts.redoLabel : ""),
  };
}

/** What the canvas session registers for the override stack: its facts, its operations, a change hook. */
export interface OverrideDomain {
  state(): StackFacts;
  undo(): boolean;
  redo(): boolean;
  subscribe(listener: () => void): () => void;
}

/** The slice of the page's routing a view may call: register (or drop) the override stack. */
export interface OverrideDomainHost {
  setOverrideDomain(domain: OverrideDomain | null): void;
}

/** The two histories' operations, supplied by the page. Each returns whether it changed anything. */
export interface EditDomains {
  override: StackFacts & { undo(): boolean; redo(): boolean };
  text: StackFacts & { undo(): boolean; redo(): boolean };
}

/**
 * Runs an Undo or Redo at `focus`: the target's own operation, and nothing for `native` (the chord is
 * never intercepted there, and a menu click cannot reach it because the click blurred the input).
 */
export function runEdit(
  kind: "undo" | "redo",
  focus: EditFocus,
  last: "override" | "text" | null,
  domains: EditDomains,
): { target: EditTarget; changed: boolean } {
  const target = targetFor(focus, last);
  if (target === "native") {
    return { target, changed: false };
  }
  return { target, changed: domains[target][kind]() };
}
