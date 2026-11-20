// SPDX-License-Identifier: MIT
// WCAG 2.2 contrast of the Instrument token pairs, per theme, read straight from
// packages/tessera-ui/themes/instrument.theme.json. Text needs 4.5:1 (1.4.3; 7:1 in the high-contrast
// theme, matching tests/unit/instrument_theme_tests.cpp), non-text indicators and control boundaries
// need 3:1 (1.4.11). "library" pairs are the ones the C++ test pins; "mock" pairs are extra pairs this
// mock actually paints. Prints every pair and exits 1 if any fails. It never edits the theme.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const themeFile = join(here, "..", "..", "..", "..", "packages", "tessera-ui", "themes",
  "instrument.theme.json");

const library = {
  text: [
    ["--on-surface", "--surface"], ["--on-surface", "--surface-raised"],
    ["--on-surface", "--surface-sunken"], ["--on-surface-muted", "--surface"],
    ["--on-surface-muted", "--surface-raised"], ["--on-surface-muted", "--surface-sunken"],
    ["--on-accent", "--accent"], ["--on-danger", "--danger"], ["--on-success", "--success"],
    ["--on-warning", "--warning"],
  ],
  indicator: [
    ["--accent", "--surface"], ["--focus-ring", "--surface"], ["--focus-ring", "--surface-raised"],
    ["--border-strong", "--surface"], ["--danger", "--surface"], ["--success", "--surface"],
    ["--warning", "--surface"],
  ],
};

const mock = {
  // Semantic colours used as text: links and the selected tab/nav item (accent), validation errors
  // (danger), deltas and status words (success, warning), on every surface they appear on.
  text: [
    ["--window-foreground", "--window-background"],
    ["--accent", "--surface"], ["--accent", "--surface-raised"], ["--accent", "--surface-sunken"],
    ["--danger", "--surface"], ["--danger", "--surface-raised"],
    ["--success", "--surface"], ["--success", "--surface-raised"],
    ["--warning", "--surface"], ["--warning", "--surface-raised"],
  ],
  // Control boundaries (text input, select, checkbox, switch track) and fills on other surfaces.
  indicator: [
    ["--border", "--surface"], ["--border", "--surface-raised"],
    ["--border-strong", "--surface-raised"], ["--border-strong", "--surface-sunken"],
    ["--accent", "--surface-raised"], ["--accent", "--surface-sunken"],
    ["--focus-ring", "--surface-sunken"], ["--focus-ring", "--accent"],
  ],
};

function parseHex(value) {
  const m = /^#([0-9a-f]{6})$/i.exec(value.trim());
  if (!m) throw new Error(`not a #rrggbb colour: ${value}`);
  const n = parseInt(m[1], 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

function luminance([r, g, b]) {
  const lin = (c) => {
    const s = c / 255;
    return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

export function contrast(a, b) {
  const [hi, lo] = [luminance(parseHex(a)), luminance(parseHex(b))].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

const theme = JSON.parse(readFileSync(themeFile, "utf8"));
const failures = [];
for (const [id, tokens] of Object.entries(theme.themes)) {
  const highContrast = id.includes("high-contrast");
  console.log(`\n${id}`);
  for (const [origin, groups] of [["library", library], ["mock", mock]]) {
    for (const [kind, pairs] of Object.entries(groups)) {
      const minimum = kind === "text" ? (highContrast ? 7 : 4.5) : 3;
      for (const [fg, bg] of pairs) {
        const ratio = contrast(tokens[fg], tokens[bg]);
        const ok = ratio >= minimum;
        const line = `${ok ? "  ok  " : "  FAIL"} ${origin.padEnd(7)} ${kind.padEnd(9)} ` +
          `${fg} on ${bg}`.padEnd(46) + ` ${ratio.toFixed(2)}:1 (min ${minimum})`;
        console.log(line);
        if (!ok) failures.push(`${id}: ${fg} on ${bg} ${ratio.toFixed(2)}:1 < ${minimum}`);
      }
    }
  }
}
console.log(failures.length === 0 ? "\ncontrast: all pairs pass" :
  `\ncontrast: ${failures.length} failing pair(s):\n  ${failures.join("\n  ")}`);
process.exit(failures.length === 0 ? 0 : 1);
