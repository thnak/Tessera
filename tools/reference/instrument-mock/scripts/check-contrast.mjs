// SPDX-License-Identifier: MIT
// WCAG 2.2 contrast per theme, read from packages/tessera-ui/themes/instrument.theme.json plus this
// mock's tokens/material.tokens.json. Text needs 4.5:1 (1.4.3; 7:1 in high contrast, as
// tests/unit/instrument_theme_tests.cpp asks), indicators and control edges 3:1 (1.4.11).
//   library  - the pairs the C++ theme test pins
//   material - pairs the reworked mock paints; text on a gradient is checked at BOTH stops, so the
//              worst stop decides. These decide the exit code.
//   finding  - theme pairs that fail and that the mock now avoids (kept to document the theme gap);
//              reported, never fatal.
// Prints every pair and exits 1 if a library or material pair fails. It never edits a token file.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const appRoot = join(here, "..");
const themeFile = join(appRoot, "..", "..", "..", "packages", "tessera-ui", "themes", "instrument.theme.json");
const materialFile = join(appRoot, "tokens", "material.tokens.json");

// [foreground, [backgrounds...]]: every background listed is a stop the text can sit on.
const groups = {
  library: {
    text: [
      ["--on-surface", ["--surface", "--surface-raised", "--surface-sunken"]],
      ["--on-surface-muted", ["--surface", "--surface-raised", "--surface-sunken"]],
      ["--on-accent", ["--accent"]], ["--on-danger", ["--danger"]],
      ["--on-success", ["--success"]], ["--on-warning", ["--warning"]],
    ],
    indicator: [
      ["--accent", ["--surface"]], ["--focus-ring", ["--surface", "--surface-raised"]],
      ["--border-strong", ["--surface"]], ["--danger", ["--surface"]],
      ["--success", ["--surface"]], ["--warning", ["--surface"]],
    ],
  },
  material: {
    text: [
      // Body and headings on the chassis, plates, raised keys and recessed trays (both stops each).
      ["--on-surface", ["--mat-chassis-a", "--mat-chassis-b", "--mat-level-0", "--mat-level-1",
        "--mat-level-2", "--mat-level-3", "--mat-level-4", "--mat-raised-a", "--mat-raised-b",
        "--mat-recess-a", "--mat-recess-b"]],
      // Micro labels (11px mono uppercase) on the same surfaces.
      ["--on-surface-muted", ["--mat-chassis-a", "--mat-chassis-b", "--mat-level-3", "--mat-level-4",
        "--mat-raised-a", "--mat-raised-b", "--mat-recess-a", "--mat-recess-b"]],
      // Accent-coloured text (selected tab, current nav item) on the chassis and trays.
      ["--accent", ["--mat-chassis-b", "--mat-raised-b", "--mat-level-1"]],
      ["--danger", ["--mat-chassis-b", "--mat-level-1"]],
      // LCD wells.
      ["--mat-on-well", ["--mat-well-a", "--mat-well-b"]],
      ["--mat-on-well-muted", ["--mat-well-a", "--mat-well-b"]],
      ["--mat-signal", ["--mat-well-a", "--mat-well-b"]],
      ["--mat-signal-warn", ["--mat-well-a", "--mat-well-b"]],
      // Inverse band (app bar, macro band, status bar).
      ["--mat-on-inverse", ["--mat-inverse-a", "--mat-inverse-b"]],
      ["--mat-on-inverse-muted", ["--mat-inverse-a", "--mat-inverse-b"]],
      ["--mat-signal", ["--mat-inverse-a", "--mat-inverse-b"]],
      // Labels on filled keys.
      ["--on-accent", ["--mat-accent-a", "--mat-accent-b"]],
      ["--on-danger", ["--mat-danger-a", "--mat-danger-b"]],
      ["--mat-on-inverse", ["--mat-dark-key-a", "--mat-dark-key-b"]],
    ],
    indicator: [
      // The focus ring is drawn outside a gap ring, so it must clear the gap, and the gap must clear
      // whatever the control is (handled by the gap colour being the lightest/darkest step).
      ["--mat-focus-ring", ["--mat-focus-gap", "--mat-chassis-a", "--mat-chassis-b", "--mat-level-4"]],
      ["--mat-focus-ring-inverse", ["--mat-inverse-a", "--mat-inverse-b", "--mat-well-b"]],
      ["--mat-focus-gap", ["--mat-accent-a", "--mat-accent-b", "--mat-danger-a"]],
      // Control edges: text fields are recessed, so their outline is the strong border.
      ["--border-strong", ["--mat-level-0", "--mat-recess-a"]],
      // Filled state of checkbox/switch/slider and LEDs on their trays.
      ["--accent", ["--mat-recess-a", "--mat-recess-b", "--mat-chassis-b"]],
      ["--mat-signal", ["--mat-well-b", "--mat-inverse-b"]],
      ["--mat-led-ok", ["--mat-inverse-a", "--mat-inverse-b"]],
      ["--mat-knob-pointer", ["--mat-knob-cap-b", "--mat-knob-cap-c"]],
    ],
  },
  finding: {
    indicator: [
      ["--border", ["--surface", "--surface-raised"]],
      ["--focus-ring", ["--accent"]],
    ],
  },
};

function parseHex(value) {
  const m = /^#([0-9a-f]{6})$/i.exec(String(value).trim());
  return m ? [0, 8, 16].map((s) => (parseInt(m[1], 16) >> (16 - s)) & 255) : null;
}

function luminance([r, g, b]) {
  const lin = (c) => (c / 255 <= 0.04045 ? c / 255 / 12.92 : ((c / 255 + 0.055) / 1.055) ** 2.4);
  return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

function contrast(a, b) {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

const theme = JSON.parse(readFileSync(themeFile, "utf8"));
const material = JSON.parse(readFileSync(materialFile, "utf8"));
const failures = [];
const known = [];
for (const [id, base] of Object.entries(theme.themes)) {
  const tokens = { ...base, ...material.shared, ...material.themes[id] };
  const highContrast = id.includes("high-contrast");
  console.log(`\n${id}`);
  for (const [origin, kinds] of Object.entries(groups)) {
    for (const [kind, pairs] of Object.entries(kinds)) {
      const minimum = kind === "text" ? (highContrast ? 7 : 4.5) : 3;
      for (const [fg, backgrounds] of pairs) {
        for (const bg of backgrounds) {
          const a = parseHex(tokens[fg]);
          const b = parseHex(tokens[bg]);
          if (!a || !b) throw new Error(`${id}: ${fg} or ${bg} is missing or not #rrggbb`);
          const ratio = contrast(a, b);
          const ok = ratio >= minimum;
          const tag = ok ? "  ok  " : origin === "finding" ? "  known" : "  FAIL";
          console.log(`${tag} ${origin.padEnd(8)} ${kind.padEnd(9)} ${`${fg} on ${bg}`.padEnd(52)} ` +
            `${ratio.toFixed(2)}:1 (min ${minimum})`);
          const line = `${id}: ${fg} on ${bg} ${ratio.toFixed(2)}:1 < ${minimum}`;
          if (!ok) (origin === "finding" ? known : failures).push(line);
        }
      }
    }
  }
}
if (known.length) console.log(`\nknown theme findings (not fatal):\n  ${known.join("\n  ")}`);
console.log(failures.length === 0 ? "\ncontrast: all library and material pairs pass" :
  `\ncontrast: ${failures.length} failing pair(s):\n  ${failures.join("\n  ")}`);
process.exit(failures.length === 0 ? 0 : 1);
