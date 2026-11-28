# Inline size runs (sub/sup/small) and the list-look goldens

## What changed

- **#1004, first half (metric spans).** `TextStyle::size_runs` (an interned `TextSizeRunsId`, the `FontFeaturesId` pattern) carries byte ranges measured and drawn at a fraction of the paragraph size. The wrap measures each run piecewise, layout (`LayoutInput::text_runs`, layout signature, retained text-change check) and paint (`paint_spanned_text`) build the same table from the spans' computed font size, and the measure cache keys it. `sub`, `sup` and `small` move from partial to mapped (alias rows, schema golden, elements-basic 2.1). Design note: `docs/design/inline-size-runs.md`. Shrink only: a span not below the paragraph size is ignored and the line box stays the paragraph's.
- **#1006.** Gallery `lists` page (ListsPage.tsx, lists.css) and `lists_light` / `lists_dark` goldens generated on lavapipe (thnak-server, GCC 15): nested disc/circle/square, decimal, start, reversed, li value, roman, alpha, plus a sub/sup/small line.
- **Two latent bugs found by the page.** Codegen read a span's id after `add_child(std::move(...))` (null dereference, segfault in any native build with an inline span); fixed with a regression check. The IR validator rejected the `font-size` that `sub`/`sup`/`small` inject (TESSERA_IR_SPAN); `font-size` is no longer a span metric property in the IR (letter-spacing and white-space still are).

## Verification

- Windows: text_size_runs, paint_text_span (new shrunk-span case). gates.py --quiet 15/15, clang-format 22.1.3 on every changed C++ file.
- Linux (lavapipe, GCC 15): full build and ctest: all pass except the schema golden, regenerated and committed. gallery_golden_lists_* and gallery_page_table pass.

## What is left

- **#1004 narrowed (not closed):** the first-line inline-start box for `list-style-position: inside` and `list-style: url()`. The issue body now describes only that.
- #1005 (::marker, counters) not touched, no design note needed.
- TextLayoutCache does not carry size_runs; the spanned paint path does not use it.

Refs #1004
Closes #1006
