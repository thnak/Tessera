// SPDX-License-Identifier: MIT

// The one DOM builder the engine-facing sessions use (the diagnostics list, the scene snapshot, the
// state cells).
//
// **Every string goes through `textContent`, never `innerHTML`.** That matters more than usual here:
// the text being quoted is the visitor's own source, and a diagnostic message quotes it. A `<` in a
// TSX file must never become markup in the page that is compiling it.
//
// React does not build these nodes. They are documents the Wasm modules produce, and they belong to the
// session that owns the pane (see `lib/views.ts` for why). Building them with three lines of DOM here
// keeps React out of a subtree it would otherwise have to reconcile on every compile.

/**
 * An element with an optional class and text. `text` is set with `textContent`, so it is escaped by the
 * DOM rather than by us -- the reason this helper exists at all.
 */
export function element<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string | null,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text !== undefined) {
    node.textContent = text;
  }
  return node;
}

/** The "module did not load" / "the compiler failed" box: one shape for all three views. */
export function unavailableBox(heading: string, body: string, detail?: string): HTMLElement {
  const box = element("div", "unavailable");
  box.append(element("h3", null, heading), element("p", null, body));
  if (detail) {
    box.append(element("p", null, "Detail: " + detail));
  }
  return box;
}
