#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One-shot: re-apply the two budget re-pins and the PaintData size pin for issue #734.

The rebase onto a newer `main` dropped the commits that carried these, so this redoes them from the
measured values. `sizeof(PaintData)` is 520 and `paint_slot_bytes` 528 after the enum base types
removed the last 8 bytes of padding; both were 8 higher before.
"""

from __future__ import annotations

import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def main() -> int:
    budget = REPO / "tests/golden/perf/memory_budget.json"
    text = budget.read_text(encoding="utf-8")

    # sizeof_paint_data: 528 -> 520. Anchored on the row's own `measures` text so it cannot hit
    # another row that happens to carry the same number.
    old = '"budget": 528,\n      "status": "enforce",\n      "unit": "bytes",\n      "measures": "sizeof(ui::PaintData)'
    new = '"budget": 520,\n      "status": "enforce",\n      "unit": "bytes",\n      "measures": "sizeof(ui::PaintData)'
    if old not in text:
        print("error: sizeof_paint_data row not found in the expected shape", file=sys.stderr)
        return 1
    text = text.replace(old, new, 1)

    # paint_slot_bytes: 536 -> 528.
    old = '"budget": 536,\n      "status": "enforce",\n      "unit": "bytes",\n      "measures": "bytes a text node'
    new = '"budget": 528,\n      "status": "enforce",\n      "unit": "bytes",\n      "measures": "bytes a text node'
    if old not in text:
        print("error: paint_slot_bytes row not found in the expected shape", file=sys.stderr)
        return 1
    text = text.replace(old, new, 1)

    text = text.replace(
        "which removed 8 bytes of internal padding. See paint_slot_bytes",
        "which removed 8 bytes of internal padding; 520 since issue #734 gave the enums explicit "
        "base types, which removed the last 8. See paint_slot_bytes",
    )
    text = text.replace(
        "which removed those 8 bytes of padding again",
        "which removed those 8 bytes of padding again; 528 since issue #734 gave the enums explicit "
        "base types",
    )
    budget.write_text(text, encoding="utf-8", newline="")

    padding = REPO / "tests/unit/padding_size_tests.cpp"
    source = padding.read_text(encoding="utf-8")
    source = source.replace(
        "constexpr std::size_t kPaintDataBytes = 528;",
        "constexpr std::size_t kPaintDataBytes = 520;",
    )
    source = source.replace(
        '"sizeof(PaintData) regressed (issue #731: LinearGradient reordered)"',
        '"sizeof(PaintData) regressed (issue #731: LinearGradient reordered; 528 -> 520 is "\n'
        '              "issue #734, the enum base types)"',
    )
    padding.write_text(source, encoding="utf-8", newline="")

    print("re-applied: sizeof_paint_data 520, paint_slot_bytes 528, kPaintDataBytes 520")
    return 0


if __name__ == "__main__":
    sys.exit(main())
