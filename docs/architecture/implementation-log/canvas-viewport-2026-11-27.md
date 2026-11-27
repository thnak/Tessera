# The Studio canvas viewport, 2026-11-27

Part of the [implementation log](../implementation-log.md). One day, four issues, one path: the
Studio sizes its canvas from a typed size, a device profile, a raw `viewport.set` and a drag, and
each of those had a defect that only a real renderer or a real browser could show.

The Studio sizes its canvas from a typed size, a device profile, and a raw `viewport.set`, and three
separate defects sat on that path. They were fixed in order, each with the evidence that it was real.

**The clamp (#690 slice a).** `canvas_viewport_request` refused a non-positive extent and passed
everything else, so a 4096x4096 size at DPR 4 asked for 1 GiB of backing store; the failure that
produces is a lost device, not a clamped preview. One pure rule now holds every path to extent
1..8192, DPR 0.01..4 and a 256 MiB ceiling. Over a limit is **clamped and reported** (`clamped` in the
report, named in the pane); a value that names no size is **refused**. The byte cap reduces the DPR,
never the logical extent, so a clamp cannot move a breakpoint. The page needs its own copy because it
sizes the store itself; the two are pinned to one table by ctest `canvas_viewport_limit` and
`preview_viewport_limits.mjs`.

**The device scale (#599).** The layout side was told the ratio and the paint side never was, so a
DPR-2 canvas painted 1:1 into the top-left quarter. `scene_frame_paint_tree()` now composes it onto
positions, border snapping and the vector extent, as `src/app/window_frame.cpp` does; at 1.0 the
transform is skipped and the goldens are byte-identical. The guard is a parity `dpr` case, and getting
that test right took two silent passes worth recording: the case had no fill (the CLI loads no
stylesheet file), and the assertion looked for *absence of the clear colour*, which a frame that
painted nothing satisfies. It now asserts the box's own colour, and reverting `set_content_scale`
turns it red.

**The page half, and one issue that did not reproduce (#598).** #598 reported the store stuck at
320x240 and named its own suspect: the React-owned `width`/`height` attributes. Restoring them changed
nothing - the session writes after React's commit and React never re-renders that leaf - so they were
put back rather than removed on a theory. `preview_browser_canvas_size.mjs` now pins the property
instead: driving the Device menu in headless Chromium, the store follows the viewport, the CSS box
keeps the logical size, and both agree with the reported DPR at DPR 1 and 2.

Two limits found the hard way. A presented WebGPU frame cannot be read back in headless Chromium
(`drawImage` and `screenshot()` both return zeros), so the DPR-2 *pixels* are pinned on lavapipe and the
browser gate stops at geometry. And the device controls are menu items whose DOM ids are derived from
their row index, so a gate must drive them by label. `tessera_canvas_draw` also took the logical
rectangle rather than the physical extent the page sized; the two agreed only while both derived the
same scale, and it now takes what `sizeToDevicePixels` produced.

### Late: the `-webkit-mask*` alias family closes #617 (2026-11-27)

The last open item on #617 was the prefixed spelling a real stylesheet reaches for. It is an alias,
not a second implementation: `mask-image` and its seven longhands are implemented once, and a second
registered name would mean a second computed value and two spellings that can disagree.

The family is a closed list folded at the registry's expand step, copying the `-webkit-backdrop-filter`
precedent. The one subtlety is the shorthand: `mask` has a parser of its own, so `-webkit-mask` is
mapped to `mask` *before* the registry dispatches. An alias applied after the dispatch would have sent
the prefixed shorthand down the generic single-value path, where an invalid layer list is accepted as
an image - a silent wrong answer rather than a diagnostic. `webkit_mask_alias` asserts the prefixed and
canonical expansions are equal, including for values `mask` rejects, and that a typo
(`-webkit-mask-compostite`) keeps its UnsupportedProperty rather than being rewritten into something
that exists.

Registering eight names tripped three gates that exist to stop exactly that drift, which is worth
noting because each is a rule rather than a chore: `schema_style_registry` wants a row per longhand
(`src/schema/src/style.cpp`, count 139 -> 147), `property_status` wants a status row per registered
property, and the matrix test walks `stylesheet-v1.properties.txt` in both directions. The `mask`
shorthand is absent from the schema table by design - that table is the TSX `style` surface, which
carries longhands only - so `-webkit-mask` is too.

### Late: the canvas drag-resize handles (#690 slice b)

The canvas' size had two authors and now has a gesture. Three grips on the canvas' own edges, a live
readout, and the rate rule the gesture exists to keep: one step per display refresh rather than one
per pointer event, plus exactly one settled step on pointer-up.

Four bugs, all found by running the thing rather than reading it:

1. The grips rendered but did nothing. The session is created from refs that exist at the first
   settle, which is before React commits the pane's grips, so `getElementById` at construction
   returned null. Fixed by delegating from the document in the capture phase, which also makes the
   grip win the press before the engine's pointer path sees it. The readout had the identical bug and
   is now resolved on first write.
2. Every drag was halved at DPR 2. The travel was divided by the device pixel ratio, but the canvas'
   CSS box is the *logical* viewport - the DPR only sizes the backing store. The divisor is the
   pane's zoom.
3. The zoom was read from the canvas, where a CSS transform scales the rect and the layout box alike,
   so the ratio was always 1. It is read from the stage the pane transforms.
4. The browser gate's frame assertion could not tell coalesced from per-move steps, because the
   session's own `requestFrame()` already coalesces frames. Mutation-checking it (replacing
   `schedule()` with a direct step) left it green. The claim moved to Node, where a burst inside one
   frame is observable, and the page gate now asserts what a person can see: the burst costs at most a
   frame per paint, and the released size is exact. That one *was* mutation-checked - removing the
   `apply` call turns it red.
