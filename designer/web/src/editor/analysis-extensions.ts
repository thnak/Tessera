// SPDX-License-Identifier: MIT

// The CodeMirror side of code intelligence (#984, #988): a hover tooltip and a completion source over the
// analysis hub. Both are best-effort: a hover or a completion that the worker cannot answer (no module, a
// superseded request, a file kind it does not read) is simply absent, never an error in the editor.

import { autocompletion, type CompletionContext, type CompletionResult } from "@codemirror/autocomplete";
import type { EditorState, Extension } from "@codemirror/state";
import { hoverTooltip } from "@codemirror/view";
import type { HoverCard } from "../lib/analysis-nav";
import type { AnalysisHub, Position } from "./analysis-hub";

const TRIGGERS = new Set(["<", ".", "/", '"', ":", " "]);

const position = (state: EditorState, path: string, offset: number): Position => ({ path, text: state.doc.toString(), offset });

/** The tooltip's content as text nodes only: a doc comment or a message is never parsed as markup. */
export function hoverDom(card: HoverCard): HTMLElement {
  const dom = document.createElement("div");
  dom.className = "cm-analysis-hover";
  const add = (tag: string, className: string, text: string): void => {
    if (text !== "") {
      const el = document.createElement(tag);
      el.className = className;
      el.textContent = text;
      dom.append(el);
    }
  };
  add("div", "cm-analysis-hover-title", card.title);
  add("pre", "cm-analysis-hover-signature", card.signature);
  add("div", "cm-analysis-hover-doc", card.doc);
  for (const problem of card.problems) {
    add("div", "cm-analysis-hover-problem", problem);
  }
  return dom;
}

export function analysisExtensions(hub: AnalysisHub, pathOf: () => string): Extension[] {
  return [
    hoverTooltip(
      async (view, pos) => {
        const card = await hub.hover(position(view.state, pathOf(), pos));
        if (card === null || (card.title === "" && card.problems.length === 0)) {
          return null;
        }
        return { pos: card.from, end: card.to, above: true, create: () => ({ dom: hoverDom(card) }) };
      },
      { hoverTime: 300 },
    ),
    autocompletion({
      override: [
        async (context: CompletionContext): Promise<CompletionResult | null> => {
          const before = context.state.doc.sliceString(Math.max(0, context.pos - 1), context.pos);
          if (!context.explicit && !/[\w$]/.test(before) && !TRIGGERS.has(before)) {
            return null;
          }
          const answer = await hub.complete(position(context.state, pathOf(), context.pos));
          if (answer === null || context.aborted) {
            return null;
          }
          return { from: answer.from, to: answer.to, options: answer.options, validFor: /^[\w$-]*$/ };
        },
      ],
    }),
  ];
}
