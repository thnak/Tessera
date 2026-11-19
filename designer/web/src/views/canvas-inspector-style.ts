// SPDX-License-Identifier: MIT

// The inspector's write half (issue #585, designer-protocol-v1 sections 9.2-9.3 and 10): the rows that set,
// clear and revert a style override on the selected node, the colour control, the refusal a bad value
// returns, and the commit section with the answer of its round trip.
//
// **Overridden values are not authored values, and the panel says so.** A computed row whose property has an
// override carries a badge and `data-origin="override"`, the override list shows every uncommitted value with
// its own Clear, and the whole section is headed "uncommitted": an override lives only in the session until a
// commit writes it to the project (section 9.2). Colour is never the only cue: the badge is text.
//
// **A refused value shows its diagnostic code inline.** The stylesheet engine's `TESSERA_CSS_*` answer is the
// compiler's own, so it is shown verbatim next to the field. It is deliberately not published to the Problems
// panel: that panel lists the compiler's diagnostics of the project files, replaced wholesale by every
// diagnostics run, and a refused override has no file or span to point at, so it would be wiped by the next
// keystroke and could not be clicked through. The diagnostics output contract is untouched.
//
// Pure DOM building from a deps record; the controller (`canvas-inspector.ts`) owns state and the module.

import { hexOf, isColorProperty } from "../lib/color-hex";
import type {
  CommitDestination,
  CommitOutcome,
  OverrideDiagnostic,
  OverrideResult,
  OverrideClient,
} from "../lib/override-client";

export interface StyleSectionDeps {
  client: OverrideClient;
  /** The selected node's decimal id and the words the history uses for it (`Button`). */
  node: string;
  nodeLabel: string;
  computed: Record<string, string>;
  /** The node's own overrides, from `style.info`. */
  overrides: readonly { property: string; value: string }[];
  /** The diagnostics the last refused value returned, or `null`. */
  refusal: readonly OverrideDiagnostic[] | null;
  /** An operation ran (accepted or refused): the controller shows the result and asks for a frame. */
  onResult: (result: OverrideResult) => void;
}

function element<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  node.className = className;
  if (text !== undefined) {
    node.textContent = text;
  }
  return node;
}

function badge(text: string): HTMLElement {
  const node = element("span", "inspect-badge", text);
  return node;
}

function button(label: string, className: string, onClick: () => void, aria?: string): HTMLButtonElement {
  const node = element("button", className, label);
  node.type = "button";
  if (aria !== undefined) {
    node.setAttribute("aria-label", aria);
  }
  node.addEventListener("click", onClick);
  return node;
}

/** The computed-style table: authored values plain, overridden ones badged, colours with a swatch control. */
export function computedBlock(deps: StyleSectionDeps): HTMLElement | null {
  const names = Object.keys(deps.computed).sort();
  if (names.length === 0) {
    return null;
  }
  const overridden = new Set(deps.overrides.map((entry) => entry.property));
  const block = element("div", "inspect-computed");
  block.appendChild(element("div", "inspect-section", "computed"));
  for (const property of names) {
    const value = deps.computed[property];
    const isOverridden = overridden.has(property);
    const row = element("div", isOverridden ? "inspect-row is-overridden" : "inspect-row");
    row.dataset.property = property;
    row.dataset.origin = isOverridden ? "override" : "authored";
    row.appendChild(element("span", "inspect-name", property));
    row.appendChild(element("span", "inspect-value", value));
    if (isOverridden) {
      row.appendChild(badge("override"));
    }
    const hex = isColorProperty(property) ? hexOf(value) : null;
    if (hex !== null) {
      const swatch = element("input", "inspect-swatch");
      swatch.type = "color";
      swatch.value = hex;
      swatch.dataset.field = `swatch:${property}`;
      swatch.setAttribute("aria-label", `Set ${property} override`);
      // `change`, not `input`: dragging inside the picker fires `input` continuously, and every one would be
      // an override operation and a history entry. `change` is the committed pick.
      swatch.addEventListener("change", () =>
        deps.onResult(
          deps.client.set(deps.node, property, swatch.value, `Set ${property} on ${deps.nodeLabel}`),
        ),
      );
      row.appendChild(swatch);
    }
    block.appendChild(row);
  }
  return block;
}

/** The override list and the set/revert form. */
export function overridesBlock(deps: StyleSectionDeps): HTMLElement {
  const block = element("div", "inspect-overrides");
  block.dataset.count = String(deps.overrides.length);
  const title = element("div", "inspect-section", "style overrides");
  if (deps.overrides.length > 0) {
    title.appendChild(badge("uncommitted"));
  }
  block.appendChild(title);

  for (const entry of deps.overrides) {
    const row = element("div", "inspect-row inspect-override");
    row.dataset.property = entry.property;
    row.dataset.origin = "override";
    row.appendChild(element("span", "inspect-name", entry.property));
    row.appendChild(element("span", "inspect-value", entry.value));
    row.appendChild(
      button(
        "Clear",
        "inspect-clear",
        () =>
          deps.onResult(
            deps.client.clear(deps.node, entry.property, `Clear ${entry.property} on ${deps.nodeLabel}`),
          ),
        `Clear ${entry.property} override`,
      ),
    );
    block.appendChild(row);
  }

  const form = element("div", "inspect-override-form");
  form.setAttribute("role", "group");
  form.setAttribute("aria-label", "Set a style override");
  const property = element("input", "inspect-field");
  property.type = "text";
  property.dataset.field = "property";
  property.placeholder = "property";
  property.setAttribute("aria-label", "Override property");
  property.setAttribute("autocapitalize", "off");
  property.spellcheck = false;
  const value = element("input", "inspect-field");
  value.type = "text";
  value.dataset.field = "value";
  value.placeholder = "value";
  value.setAttribute("aria-label", "Override value");
  value.spellcheck = false;
  // A value is ONE override operation, applied on Enter or on the Set button; typing in the field is the
  // browser's own text undo (D3), and nothing reaches the override stack until then.
  const apply = (): void => {
    if (property.value.trim() === "") {
      return;
    }
    deps.onResult(
      deps.client.set(
        deps.node,
        property.value.trim(),
        value.value,
        `Set ${property.value.trim()} on ${deps.nodeLabel}`,
      ),
    );
  };
  for (const field of [property, value]) {
    field.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        apply();
      }
    });
    // Blur after an edit is the other way a value ends (`change`): applied only when both fields hold text, so
    // tabbing through an empty form does not raise a refusal. A repeat of the same value is the module's no-op.
    field.addEventListener("change", () => {
      if (property.value.trim() !== "" && value.value.trim() !== "") {
        apply();
      }
    });
  }
  form.append(property, value, button("Set", "inspect-set", apply, "Set override"));
  block.appendChild(form);

  if (deps.refusal !== null && deps.refusal.length > 0) {
    for (const diagnostic of deps.refusal) {
      const note = element("div", "inspect-diagnostic", `${diagnostic.code}: ${diagnostic.message}`);
      note.setAttribute("role", "alert");
      note.dataset.code = diagnostic.code;
      block.appendChild(note);
    }
  }

  const revert = button("Revert all", "inspect-revert", () => deps.onResult(deps.client.revert()));
  revert.setAttribute("aria-label", "Revert every override in the preview");
  if (deps.client.state().overrides.length === 0) {
    revert.setAttribute("aria-disabled", "true");
  }
  block.appendChild(revert);
  return block;
}

export interface CommitSectionDeps {
  /** The project's stylesheet paths, for the `class` destination's file. */
  stylesheets: readonly string[];
  /** The first overridden property's computed value: the default for the `token` destination's value. */
  tokenValue: string;
  hasOverrides: boolean;
  outcome: CommitOutcome | null;
  onCommit: (
    destination: CommitDestination,
    fields: { className: string; file: string; token: string; tokenValue: string },
  ) => void;
}

/** The sentence a commit's outcome shows; the codes are the protocol's, verbatim, so they can be looked up. */
export function outcomeText(outcome: CommitOutcome): string {
  switch (outcome.status) {
    case "committed":
      return `committed: ${outcome.edits} edit${outcome.edits === 1 ? "" : "s"} in ${outcome.files.length} file${
        outcome.files.length === 1 ? "" : "s"
      }; the overrides are now the source`;
    case "no-patch":
      return `no patch: ${outcome.reason}${outcome.detail === "" ? "" : ` — ${outcome.detail}`}`;
    case "rejected":
      return `patch rejected (${outcome.reason}): ${outcome.detail}; overrides kept`;
    case "mismatch":
      return `applied, but ${outcome.diagnostics.map((d) => `${d.code}: ${d.message}`).join("; ")}; overrides kept`;
    case "unsettled":
      return "applied; waiting for the source to compile before the overrides settle";
    case "busy":
      return "a commit for this node is already open";
  }
}

/** The commit section: one button per destination, their options, and the answer of the last commit. */
export function commitBlock(deps: CommitSectionDeps): HTMLElement {
  const block = element("div", "inspect-commit");
  block.appendChild(element("div", "inspect-section", "commit"));

  const options = element("div", "inspect-commit-options");
  const className = element("input", "inspect-field");
  className.type = "text";
  className.dataset.field = "className";
  className.placeholder = "class name";
  className.setAttribute("aria-label", "Class name for the class destination");
  className.spellcheck = false;
  const file = element("select", "inspect-field");
  file.dataset.field = "file";
  file.setAttribute("aria-label", "Stylesheet for the class destination");
  for (const path of deps.stylesheets) {
    const option = element("option", "", path);
    option.value = path;
    file.appendChild(option);
  }
  const token = element("input", "inspect-field");
  token.type = "text";
  token.dataset.field = "token";
  token.placeholder = "--token";
  token.setAttribute("aria-label", "Token for the token destination");
  token.spellcheck = false;
  const tokenValue = element("input", "inspect-field");
  tokenValue.type = "text";
  tokenValue.dataset.field = "tokenValue";
  tokenValue.placeholder = "token value";
  tokenValue.value = deps.tokenValue;
  tokenValue.setAttribute("aria-label", "Value the token takes");
  tokenValue.spellcheck = false;
  options.append(className, file, token, tokenValue);

  const actions = element("div", "inspect-commit-actions");
  actions.setAttribute("role", "group");
  actions.setAttribute("aria-label", "Commit destination");
  for (const destination of ["inline", "class", "token"] as const) {
    const node = button(destination, "inspect-commit-button", () =>
      deps.onCommit(destination, {
        className: className.value.trim(),
        file: file.value,
        token: token.value.trim(),
        tokenValue: tokenValue.value,
      }),
    );
    node.dataset.destination = destination;
    // An empty override layer has nothing to commit; the control stays focusable and says why.
    if (!deps.hasOverrides) {
      node.setAttribute("aria-disabled", "true");
      node.title = "Set an override first: a commit writes the node's overrides";
    }
    actions.appendChild(node);
  }
  block.append(actions, options);

  if (deps.outcome !== null) {
    const answer = element("div", "inspect-commit-answer", outcomeText(deps.outcome));
    answer.dataset.status = deps.outcome.status;
    answer.setAttribute("role", "status");
    block.appendChild(answer);
  }
  return block;
}
