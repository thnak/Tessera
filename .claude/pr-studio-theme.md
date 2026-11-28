Studio: theme and design-token editor with WCAG contrast (#582)

## What changed
- New Theme panel (`view.theme`, tab in the side bar group) edits the tokens of a `*.theme.json` asset or the `:root` custom properties of a stylesheet, one column per variant (light, dark, high contrast), with the theme-generation-v1 section 4 contrast table per variant (7:1 text and 4.5:1 non-text in high contrast); verdicts are words, not colour alone.
- `designer/web/theme-tokens.js`: span-preserving read/write, so comments, order and layout survive; `theme-contrast.js`: WCAG formula and the same role table as `contrast.hpp`.
- Writes are one editor transaction (Ctrl+Z, format, diff apply; stylesheet edits are a live apply so the Canvas repaints without Restart); the panel also has an Undo button.
- Layout/menu/command wiring; `workbench_layout.mjs` expectations updated for the fourth side bar tab.

## Verification
- `node scripts/ci/studio_theme.mjs` (known contrast vectors, shipped Instrument theme passes all three variants, round trips), added to pages.yml.
- Playwright `scripts/ci/workbench_theme.mjs` (developer-run) passes locally; workbench_layout, workbench, menus, palette, notifications, smoke, live-edit wiring pass; `gates.py --quiet` 15/15; tsc clean.

## What is left
- Theme asset (`*.theme.json`) edits take the restart path (host reads assets at start); seed-colour regeneration in the panel. Split to #1017.

Refs #582
