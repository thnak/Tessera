// SPDX-License-Identifier: MIT

// The scene view's two document renderers (the documents are P2 issue #166 and P4 issue #168; the module
// split is issue #392, the React host is issue #422).
//
// Kept apart from the session for the same reason as the diagnostics list: these are pure rendering,
// and the session has to stay inside the source-size limit while carrying the Wasm lifecycle.
//
// The shapes are the ones `tools/codegen/scene_preview.cpp` and `scene_interaction.hpp` produce, which is
// what the parity gates compare byte for byte -- so this file must not invent, sort or filter anything.
// It shows the document.

import { element, unavailableBox } from "./dom";

export { unavailableBox };

/** One node of the canonical snapshot document. */
export interface SceneNode {
  kind?: string;
  path?: string;
  depth?: number;
  text?: string;
  uiId?: string;
  bounds?: { x?: number; y?: number; width?: number; height?: number };
}

export interface SceneDocument {
  nodes?: SceneNode[];
}

// The bounds are fixed three-decimal numbers in the document; this only makes them readable.
function describeBounds(bounds: SceneNode["bounds"]): string {
  if (!bounds || typeof bounds !== "object") {
    return "";
  }
  const number = (value: unknown): string => (typeof value === "number" ? value.toFixed(0) : "?");
  return `${number(bounds.x)},${number(bounds.y)} ${number(bounds.width)}x${number(bounds.height)}`;
}

/**
 * Renders the scene snapshot's node list.
 *
 * `onPick(automationId)` is called when a row's `click` button is pressed: the row is the id picker, so a
 * reader never has to copy a 64-bit number out of the list. Returns the node count so the caller can
 * phrase its own status line.
 */
export function renderNodes(
  list: HTMLElement,
  document_: SceneDocument,
  onPick: (automationId: string) => void,
): number {
  list.replaceChildren();
  const nodes = Array.isArray(document_.nodes) ? document_.nodes : [];
  for (const node of nodes) {
    // Indented by the node's depth so the preorder list reads as a tree, without a nested <ul> per
    // level (the document is flat, and building a tree from paths would be a second source of truth).
    const row = element("li", "node");
    row.style.paddingLeft = `${8 + (Number(node.depth) || 0) * 18}px`;
    row.append(
      element("span", "node-kind", String(node.kind ?? "")),
      element("span", "node-path", String(node.path ?? "")),
      element("span", "node-bounds", describeBounds(node.bounds)),
    );
    if (node.text) {
      row.append(element("span", "node-text", String(node.text)));
    }
    if (node.uiId && node.uiId !== "0") {
      row.append(element("span", "node-id", `uiId ${node.uiId}`));
      const pick = element("button", "node-pick", "click");
      pick.type = "button";
      pick.title = `Append \`click ${node.uiId}\` to the input script`;
      pick.addEventListener("click", () => onPick(String(node.uiId)));
      row.append(pick);
    }
    list.append(row);
  }
  return nodes.length;
}

/** The "no scene" box: an empty document is a documented answer, not a successful empty page. */
export function noSceneBox(): HTMLElement {
  const note = element("div", "unavailable");
  note.append(
    element("h3", null, "No scene"),
    element(
      "p",
      null,
      "The source produced no scene. The preview evaluates the static subset; a user component " +
        "instance, a conditional or repeated child, or an inline-span Text is reported as " +
        "unsupported (docs/design/compiler-wasm.md section 7.1). Open the Diagnostics view for the " +
        "compile error, if there is one.",
    ),
  );
  return note;
}
