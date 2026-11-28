# layout: HTML table layout and table tags (ADR-059 group D)

Refs #543. Remaining gaps are tracked in #1049.

## What changed

- ADR-079 and `docs/design/table-layout.md`: a table is a carrier (`View` plus display values), not a node kind.
- `TableEngine` (`src/ui/layout/table_*.cpp`) behind the grid seam: auto and fixed layout, `colspan`/`rowspan` (incl. `rowspan=0`), header/footer group ordering, captions, `colgroup`/`col`, `border-spacing`, `border-collapse` (overlap model), percent and fixed widths, RTL, vertical-align, anonymous rows.
- The ten tags (`table`, `thead`, `tbody`, `tfoot`, `tr`, `td`, `th`, `caption`, `colgroup`, `col`) in the alias table, user-agent sheet and typings; spans reach `LayoutInput` via `aria-colspan`/`aria-rowspan`.
- New properties: `border-collapse`, `border-spacing`, `caption-side`, `table-layout`; `vertical-align` accepts top/middle/bottom.
- Roles `table`, `row`, `cell`, `columnheader` with AT-SPI, UIA and Android mappings.
- Gallery tables page with lavapipe goldens `tables_light` and `tables_dark`.

## Verification

- Windows (vs2026 Release): full ctest green apart from environment cases (codegen/icons tests need TEMP inside the repo and pass with it; `gradient_gpu` differs on the real GPU, unrelated).
- `python -B scripts/gates.py --quiet`: 15/15. clang-format 22.1.3 clean.
- New tests: `table_layout` (hand-computed corpus), `a11y_table`; updated schema, UA sheet, supports, text-decoration and mapping tests.
- Linux GCC 15 + lavapipe: see the report accompanying this PR.

## What is left (#1049)

Per-edge collapsed-border conflict resolution and single painting, caption outside the table border, anonymous table wrapper, baseline alignment, `rowheader`/`scope`/`rowgroup` roles, AT-SPI/UIA table interfaces. The `table` alias row stays `partial`.

Notes: the ADR number 079 may collide with a parallel branch; AT-SPI role numbers should be re-checked against the enum.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
