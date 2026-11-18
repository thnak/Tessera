// SPDX-License-Identifier: MIT

// The editor's look (issue #517): colours come from CSS custom properties, so the editor follows the page's
// light/dark theme with no second source of truth. The properties are defined in `styles/code.css`
// (`--ed-*`, `--tok-*`), next to the editor's other rules, where the contrast of each pair is checked.
//
// Nothing here names a colour literal: a theme change is a stylesheet change, never a rebuild of the editor.

import { HighlightStyle } from "@codemirror/language";
import { EditorView } from "@codemirror/view";
import { tags } from "@lezer/highlight";

/** Token colours: keyword, string, comment and the rest of the TSX/CSS/JSON vocabulary the studio shows. */
export const studioHighlight = HighlightStyle.define([
  { tag: [tags.keyword, tags.modifier, tags.operatorKeyword, tags.controlKeyword], color: "var(--tok-keyword)" },
  { tag: [tags.string, tags.special(tags.string), tags.regexp], color: "var(--tok-string)" },
  { tag: [tags.number, tags.bool, tags.null, tags.atom], color: "var(--tok-number)" },
  { tag: [tags.comment, tags.lineComment, tags.blockComment], color: "var(--tok-comment)", fontStyle: "italic" },
  { tag: [tags.tagName, tags.angleBracket], color: "var(--tok-tag)" },
  { tag: [tags.attributeName, tags.propertyName], color: "var(--tok-attribute)" },
  { tag: [tags.className, tags.typeName, tags.namespace], color: "var(--tok-type)" },
  { tag: [tags.function(tags.variableName), tags.function(tags.propertyName), tags.labelName], color: "var(--tok-function)" },
  { tag: [tags.definition(tags.variableName), tags.variableName], color: "var(--ed-fg)" },
  { tag: [tags.operator, tags.punctuation, tags.bracket, tags.separator], color: "var(--tok-punct)" },
  { tag: tags.invalid, color: "var(--error)" },
]);

/** The editor chrome: surface, gutter, caret, selection, the line highlight and the diagnostic underlines. */
export const studioTheme = EditorView.theme({
  "&": {
    height: "100%",
    backgroundColor: "var(--ed-bg)",
    color: "var(--ed-fg)",
    fontSize: "12px",
  },
  "&.cm-focused": { outline: "2px solid var(--accent)", outlineOffset: "-2px" },
  ".cm-scroller": { fontFamily: "var(--mono)", lineHeight: "1.65", overflow: "auto" },
  ".cm-content": { caretColor: "var(--ed-fg)", padding: "8px 0" },
  ".cm-cursor, .cm-dropCursor": { borderLeftColor: "var(--ed-fg)" },
  ".cm-gutters": {
    backgroundColor: "var(--ed-bg)",
    color: "var(--tok-comment)",
    border: "none",
  },
  ".cm-activeLine": { backgroundColor: "var(--ed-line)" },
  ".cm-activeLineGutter": { backgroundColor: "var(--ed-line)", color: "var(--ed-fg)" },
  "&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection": {
    backgroundColor: "var(--ed-selection)",
  },
  ".cm-matchingBracket": { backgroundColor: "var(--ed-selection)", outline: "none" },
  ".cm-selectionMatch": { backgroundColor: "var(--ed-selection)" },
  ".cm-searchMatch": { backgroundColor: "var(--ed-selection)" },
  ".cm-panels": { backgroundColor: "var(--pane-bg)", color: "var(--text)", borderColor: "var(--border)" },
  ".cm-diag": { textDecorationLine: "underline", textDecorationStyle: "wavy", textUnderlineOffset: "3px" },
  ".cm-diag-error": {
    textDecorationColor: "var(--error)",
    backgroundColor: "color-mix(in srgb, var(--error) 18%, transparent)",
  },
  ".cm-diag-warning": {
    textDecorationColor: "var(--warning)",
    backgroundColor: "color-mix(in srgb, var(--warning) 18%, transparent)",
  },
});
