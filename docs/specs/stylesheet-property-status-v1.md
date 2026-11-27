# Stylesheet property status (V1)

Every property the registry accepts, and what it actually does. This is the normative companion to
[`stylesheet-v1.md`](stylesheet-v1.md) section 11: that section says which *areas* V1 implements,
this one says, per registered longhand, whether a consumer reads the computed value and which test
proves it.

The registry is the source of truth for the *set* of properties
(`PropertyRegistry::longhand_names()`); this table is the source of truth for their *status*. The
two are cross-checked by `tests/unit/property_status_tests.cpp`, so a property added to the
registry without a row here, a row for a property that is not registered, or an `applied` row with
no named test all fail the test run. That is the enforcement behind issue #213: a property cannot
quietly lose its consumer.

## 1. Status values

| Status | Meaning |
|---|---|
| **applied** | A consumer reads the computed value on a real path (layout box, paint output, hit test, accessibility tree, scroll runtime, or window material). The evidence column names the test that observes the effect through that path. |
| **parsed only** | The registry accepts the property, the cascade computes it, `@supports` reports it, and it inherits or not as CSS says — but nothing reads the value, so setting it changes no pixel, box, hit test or accessible node. The reason column says why, and what would have to land for it to become **applied**. |
| **removed** | Not in the registry. No property is in this state in V1; the value exists so a future removal has a recorded place. |

A `parsed only` property is not a bug by itself: several are correct CSS behaviour for a flex-only
layout engine (`justify-items` is ignored on a flex container in CSS too). What would be a bug is a
property that is *documented* as applied and is not, which is what the cross-check prevents.

## 2. Layout

| Property | Status | Consumer / evidence |
|---|---|---|
| `display` | applied | `layout_style.cpp` maps `none`/`contents`/`flex`/`block` onto Yoga; `layout` |
| `position` | applied | `layout_style.cpp` `apply_position`; `layout` |
| `box-sizing` | applied | `layout_style.cpp` `YGNodeStyleSetBoxSizing`; `layout` |
| `width` | applied | `layout_style.cpp` `apply_dimension`; `layout` |
| `height` | applied | `layout_style.cpp` `apply_dimension`; `layout` |
| `min-width` | applied | `layout_style.cpp` `apply_dimension`; `layout` |
| `min-height` | applied | `layout_style.cpp` `apply_dimension`; `layout` |
| `max-width` | applied | `layout_style.cpp` `apply_dimension`; `layout` |
| `max-height` | applied | `layout_style.cpp` `apply_dimension`; `layout` |
| `aspect-ratio` | applied | `layout_style.cpp` (skipped when both axes are definite, issue #215); `layout` |
| `top` | applied | `layout_style.cpp` `apply_position`; `layout` |
| `right` | applied | `layout_style.cpp` `apply_position`; `layout` |
| `bottom` | applied | `layout_style.cpp` `apply_position`; `layout` |
| `left` | applied | `layout_style.cpp` `apply_position`; `layout` |
| `margin-top` | applied | `layout_style.cpp` `apply_margin`; `layout` |
| `margin-right` | applied | `layout_style.cpp` `apply_margin`; `layout` |
| `margin-bottom` | applied | `layout_style.cpp` `apply_margin`; `layout` |
| `margin-left` | applied | `layout_style.cpp` `apply_margin`; `layout` |
| `padding-top` | applied | `layout_style.cpp` `apply_padding`; `layout` |
| `padding-right` | applied | `layout_style.cpp` `apply_padding`; `layout` |
| `padding-bottom` | applied | `layout_style.cpp` `apply_padding`; `layout` |
| `padding-left` | applied | `layout_style.cpp` `apply_padding`; `layout` |
| `border-top-width` | applied | `layout_style.cpp` `apply_border` (box size) and `paint_properties.cpp` (edge); `layout` |
| `border-right-width` | applied | `layout_style.cpp` `apply_border`; `layout` |
| `border-bottom-width` | applied | `layout_style.cpp` `apply_border`; `layout` |
| `border-left-width` | applied | `layout_style.cpp` `apply_border`; `layout` |
| `border-top-style` | applied | `layout_style.cpp` `apply_border`; `layout` |
| `border-right-style` | applied | `layout_style.cpp` `apply_border`; `layout` |
| `border-bottom-style` | applied | `layout_style.cpp` `apply_border`; `layout` |
| `border-left-style` | applied | `layout_style.cpp` `apply_border`; `layout` |
| `border-top-color` | applied | `paint_properties.cpp`; `paint_pass` |
| `border-right-color` | applied | `paint_properties.cpp`; `paint_pass` |
| `border-bottom-color` | applied | `paint_properties.cpp`; `paint_pass` |
| `border-left-color` | applied | `paint_properties.cpp`; `paint_pass` |
| `border-top-left-radius` | applied | `paint_properties.cpp` corner radii; `paint_clip`; a uniform border follows it as one anti-aliased ring (`fill_rounded_ring`, `paint_pass`); per-side widths or colours keep square edge strips |
| `border-top-right-radius` | applied | `paint_properties.cpp` corner radii; `paint_clip`; a uniform border follows it as one anti-aliased ring (`fill_rounded_ring`, `paint_pass`); per-side widths or colours keep square edge strips |
| `border-bottom-right-radius` | applied | `paint_properties.cpp` corner radii; `paint_clip`; a uniform border follows it as one anti-aliased ring (`fill_rounded_ring`, `paint_pass`); per-side widths or colours keep square edge strips |
| `border-bottom-left-radius` | applied | `paint_properties.cpp` corner radii; `paint_clip`; a uniform border follows it as one anti-aliased ring (`fill_rounded_ring`, `paint_pass`); per-side widths or colours keep square edge strips |
| `flex-direction` | applied | `layout_style.cpp`; `layout` |
| `flex-wrap` | applied | `layout_style.cpp`; `layout` |
| `flex-grow` | applied | `layout_style.cpp`; `layout` |
| `flex-shrink` | applied | `layout_style.cpp`; `layout` |
| `flex-basis` | applied | `layout_style.cpp`; `layout` |
| `align-items` | applied | `layout_style.cpp`; `layout` |
| `align-self` | applied | `layout_style.cpp`; `layout` |
| `align-content` | applied | `layout_style.cpp`; `layout` |
| `justify-content` | applied | `layout_style.cpp` (flex) and `grid_tracks.cpp` (grid track alignment); `layout`, `grid_layout` |
| `justify-items` | applied | `grid_layout.cpp` supplies the default item alignment for a grid container; CSS ignores it on a flex container; `grid_layout` |
| `justify-self` | applied | `grid_layout.cpp` aligns one grid item in its area; `grid_layout` |
| `grid-template-columns` | applied | `grid_values.cpp` `parse_grid_template`; `grid_tracks.cpp` `size_axis`; `grid_layout` |
| `grid-template-rows` | applied | `grid_values.cpp` `parse_grid_template`; `grid_tracks.cpp` `size_axis`; `grid_layout` |
| `grid-column` | applied | `grid_values.cpp` `parse_grid_line`; `grid_placement.cpp` `place_items`; `grid_layout` |
| `grid-row` | applied | `grid_values.cpp` `parse_grid_line`; `grid_placement.cpp` `place_items`; `grid_layout` |
| `grid-auto-flow` | applied | `grid_placement.cpp` `place_items` cursor direction; `grid_layout` |
| `row-gap` | applied | `layout_style.cpp` (flex) and `grid_tracks.cpp` `resolve_gap` (grid); `layout`, `grid_layout` |
| `column-gap` | applied | `layout_style.cpp` (flex) and `grid_tracks.cpp` `resolve_gap` (grid); `layout`, `grid_layout` |
| `order` | applied | `layout_engine.cpp` `child_layout_order` inserts Yoga children in ascending `order` (stable, so ties keep source order) and the box walk uses the same permutation; `property_effects` |
| `field-sizing` | applied | `layout_engine.cpp` makes a `content` field a measured leaf; `layout` |
| `overflow-x` | applied | `layout_style.cpp` (Yoga overflow) and `paint_pass.cpp` (child clip); `paint_clip` |
| `overflow-y` | applied | `layout_style.cpp` (Yoga overflow) and `paint_pass.cpp` (child clip); `paint_clip` |
| `scrollbar-gutter` | applied | `layout_style.cpp` `apply_scrollbar_gutter`; `scroll_properties` |
| `direction` | applied | `layout_style.cpp` (Yoga direction) and `paint_properties.cpp` (logical edges); `logical_layout` |
| `margin-inline-start` | applied | `layout_style.cpp` Start/End edges; `logical_layout` |
| `margin-inline-end` | applied | `layout_style.cpp` Start/End edges; `logical_layout` |
| `padding-inline-start` | applied | `layout_style.cpp` Start/End edges; `logical_layout` |
| `padding-inline-end` | applied | `layout_style.cpp` Start/End edges; `logical_layout` |
| `inset-inline-start` | applied | `layout_style.cpp` `apply_position`; `logical_layout` |
| `inset-inline-end` | applied | `layout_style.cpp` `apply_position`; `logical_layout` |
| `border-inline-start-width` | applied | `layout_style.cpp` and `paint_properties.cpp`; `logical_layout` |
| `border-inline-start-style` | applied | `layout_style.cpp` and `paint_properties.cpp`; `logical_layout` |
| `border-inline-start-color` | applied | `paint_properties.cpp` `inline_color`; `logical_layout` |
| `border-inline-end-width` | applied | `layout_style.cpp` and `paint_properties.cpp`; `logical_layout` |
| `border-inline-end-style` | applied | `layout_style.cpp` and `paint_properties.cpp`; `logical_layout` |
| `border-inline-end-color` | applied | `paint_properties.cpp` `inline_color`; `logical_layout` |

## 3. Paint

| Property | Status | Consumer / evidence |
|---|---|---|
| `background-color` | applied | `paint_pass.cpp` `paint_background`; `paint_pass` |
| `background-image` | applied | a comma-separated list of gradient layers and `none`, painted bottom to top (#614); each layer is a `linear-gradient()` (2-8 stops, `deg`/`turn`/`rad`/`grad` or `to <side\|corner>`) or a `radial-gradient()` (`circle`/`ellipse`, the four size keywords or explicit radii, an `at` position), each with a `repeating-` form (#613); the `background` shorthand expands to this and `background-color`, putting the colour on the final layer only and rejecting `url()`/position/repeat/size/attachment/box with a diagnostic; `paint_gradient.cpp`, `style/background_layers.cpp`, `style/gradient_radial.cpp`, `property_background.cpp` (#457, #613, #614) |
| `mask-image` | applied | Same layer-list grammar as `background-image` (linear, radial and conic gradients, `none`; no `url()`). `paint_pass.cpp` opens an isolated layer for the node, paints it, then draws the mask image (`paint_gradient.cpp` `paint_mask_image`) into a second target and multiplies the layer by its alpha (`mask-mode: alpha`, RHI 1.19; [`offscreen-layers.md`](../design/gpu/offscreen-layers.md) 5.2). The mask covers the border box, unrounded; `mask-origin`, `mask-clip` and `mask-composite` are not implemented (the `mask` shorthand rejects them). A sink that cannot isolate or mask paints the node unmasked. A masked node is never per-node paint-cached; its mask rides in the subtree draw list. Measured against Chromium (linear, radial and conic masks, opacity 1 and 0.5): a linear ramp is within 2 of 255, radial within about 8 (32x32 cells) and conic within 5 (64x64 cells); see offscreen-layers.md 5.2, 5b. `paint_mask` |
| `mask-size` | applied | Per-layer geometry of the `mask-image` list (#617), each a comma-separated list that repeats cyclically over the layers. `mask-size`: `auto`/`cover`/`contain` (all the border box, since a gradient has no intrinsic size) or 1-2 px/% lengths; `mask-position`: 1-4 values (keywords, px, %, and the `right 10px bottom 20px` edge form); `mask-repeat`: `repeat-x`/`repeat-y` or 1-2 of `repeat`/`space`/`round`/`no-repeat`; `mask-mode`: `alpha`, `luminance` (sRGB coefficients folded into each stop's alpha before the ramp is sampled, exact for opaque stops) and `match-source` (alpha, for gradients). Lengths are px and % only (no em/rem/calc). A repeat that needs more than 256 tiles, or a zero-size tile, paints no tile for that layer; a tile reaching outside the box is clipped to it. An invalid list drops the declaration with a diagnostic. `paint_gradient.cpp`, `paint_mask_tiles.cpp`, `style/mask_layers.cpp`; `mask_layers`, `paint_mask_geometry` The `mask` shorthand (`property_mask.cpp`) expands each layer into the five longhands (every one emitted, so unnamed ones reset) and rejects `mask-origin`, `mask-clip` and `mask-composite` parts with a diagnostic. |
| `mask-position` | applied | Per-layer position of the `mask-image` tiles; grammar and behaviour under `mask-size`. `paint_mask_tiles.cpp`; `paint_mask_geometry` |
| `mask-repeat` | applied | Per-layer repeat style (`repeat-x`, `repeat-y`, 1-2 of `repeat`/`space`/`round`/`no-repeat`); see `mask-size`. `paint_mask_tiles.cpp`; `paint_mask_geometry` |
| `mask-mode` | applied | Per-layer `alpha`, `luminance` or `match-source` (alpha for gradients); see `mask-size`. `paint_gradient.cpp` `emit_gradient`; `paint_mask_geometry` |
| `mask` | applied | Shorthand: `<mask-reference> || <position> [/ <size>] || <repeat-style> || <masking-mode>` per layer, expanded to `mask-image`, `mask-position`, `mask-size`, `mask-repeat` and `mask-mode` (every longhand is emitted, so unnamed ones reset). A geometry box or a compositing operator (`mask-origin`, `mask-clip`, `mask-composite`) rejects the whole declaration with a diagnostic. `property_mask.cpp`; `mask_layers` |
| `color` | applied | `paint_pass.cpp` `current_color` (also the `currentColor` base); `paint_text` |
| `color-scheme` | applied | inherited; `var_resolve.cpp` `color_context_for` reads it to pick the `light-dark()` branch (issue #620); `color_functions` |
| `opacity` | applied | `paint_pass.cpp` effective-opacity product; `paint_opacity` |
| `box-shadow` | applied | `paint_pass.cpp` `paint_box_shadows` and `scene_style_sync.cpp` outsets; `paint_shadow`; outer shadows are carved to the rounded border box (#461) and `inset` shadows paint inside it above the background (#612); transitions and keyframes interpolate the list (`shadow_interpolation.cpp`, #462) |
| `text-shadow` | applied | `paint_text.cpp` installs `TextShadowSink` (`paint_text_shadow.cpp`) around the payload emission, so every run - wrapped lines, span slices, the ellipsized line, the preedit - gets its shadows behind its glyphs; `scene_style_sync.cpp` folds the reach into the paint outsets; transitions and keyframes interpolate the list (`interpolate_text_shadow`, #616); `paint_text_shadow`, `text_shadow_interpolation` |
| `z-index` | applied | `scene_style_sync.cpp` → `Node::set_z_index`, read by `for_each_by_z`; `z_order` |
| `clip-path` | applied | `clip_path.cpp` parses `inset()` (with `round`), `circle()` and `ellipse()` into a clip record; the fragment shader and `CpuCanvas` share `gpu::clip::covers_pixel` (ADR-068 slice 4, #672). `polygon([nonzero|evenodd,] x y, …)` parses into a `Polygon` record with a pooled vertex run (slice 5, #673; 256 vertices per polygon and 4096 per frame, over-limit degrades to the bounding box and is counted). `url()`/`path()` are out of scope and report `unsupported` rather than being silently ignored, so the gap is counted instead of painted unclipped. Transitions and keyframes interpolate same-kind shapes (`clip_path_interpolation.cpp`, #674; `inset()` edges and radii, `circle()`/`ellipse()`, `polygon()` of equal size and fill rule) and switch discretely otherwise. The paint pass does not read `clip-path` yet. `clip_path`, `clip_polygon`, `clip_path_interpolation` |
| `transform` | applied | `scene_style_sync.cpp` → `Node::set_transform`; `transform` |
| `transform-origin` | applied | `scene_style_sync.cpp` resolves the matrix around the origin; `scene_style_sync` |
| `backdrop-filter` | applied | `paint_properties.cpp` `backdrop_filter_value` → glass sigma; `glass_paint` |
| `-webkit-backdrop-filter` | applied | Registry alias of `backdrop-filter` (one computed value); `glass_style` |
| `window-backdrop` | applied | `window_backdrop_request.cpp` reads the **root node's inline** declaration and hands it to the HAL. A stylesheet rule is not read: the window material is chosen before the host exists (glass-v1 section 6). `property_effects` |

## 4. Typography

| Property | Status | Consumer / evidence |
|---|---|---|
| `font-size` | applied | `text_measure.cpp` `text_style_from`; `text_measure` |
| `font-weight` | applied | `text_measure.cpp`; `paint_text` |
| `font-style` | applied | `text_measure.cpp` sets `TextStyle::italic`; `property_effects` |
| `line-height` | applied | `text_measure.cpp`; `text_measure` |
| `letter-spacing` | applied | `text_measure.cpp`; `text_measure` |
| `word-spacing` | applied | `text_measure.cpp` (additive per painted ASCII space); `word_spacing` |
| `text-align` | applied | `paint_text.cpp` line start offset; `paint_text_decoration` |
| `text-transform` | applied | `text_measure.cpp` `parse_text_transform`; `text_transform` |
| `white-space` | applied | `text_measure.cpp` `parse_white_space`; `text_measure` |
| `text-overflow` | applied | `text_measure.cpp` `parse_text_overflow` → `paint_text_runs.cpp` emits the fitted prefix plus `…`; `property_effects` |
| `text-decoration-line` | applied | `text_measure.cpp` `text_style_from` -> `paint_text_rules.cpp` draws underline/overline/line-through rects after the glyph runs; `text_decoration` |
| `text-decoration-style` | applied | Only `solid` is accepted (other values are rejected with a diagnostic, never approximated); the rules are solid rects; `text_decoration` |
| `text-decoration-color` | applied | `paint_text_rules.cpp` `resolve_text_rules` (`currentColor` follows the text colour); `text_decoration` |
| `text-decoration-thickness` | applied | `text_measure.cpp` -> `paint_text_rules.cpp` (`auto`/`from-font` derive 1/16 em, min 1 px); `text_decoration` |
| `vertical-align` | applied | `text_measure.cpp` `baseline_shift` (`baseline`, `sub`, `super`, length) -> paint-time baseline shift of the run and its rules in `paint_text.cpp` / `paint_text_spans.cpp`; `text_decoration` |
| `list-style-type` | applied | `none`, `disc`, `circle`, `square`, `decimal`, `lower-alpha`/`lower-latin`, `upper-alpha`/`upper-latin`, `lower-roman`, `upper-roman`; other counter styles are rejected with a diagnostic. Inherited, initial `none` (no `display: list-item`; the user-agent sheet gives `ul`/`ol` their type). Paint only: `paint_list_marker.cpp` draws the shape or counter text outside the item's border box. The `list-style` shorthand (`<type> || <position>`, `none`; `url()` is rejected) expands in `property_list_style.cpp`; `list_style` |
| `list-style-position` | applied | `outside` only; `inside` is rejected with a diagnostic, never approximated (the layout cannot flow a marker into the first line). `list_style` |
| `font-family` | parsed only | The measure seam takes a `text::TextStyle`, which has no family field: V1 ships one deterministic stub/FreeType face, so a family list has nothing to select. `layout_signature.cpp` classifies it paint-only for the same reason. Becomes **applied** when the text seam grows a family field and a font stack. |

## 5. Interaction and visibility

| Property | Status | Consumer / evidence |
|---|---|---|
| `pointer-events` | applied | `interaction_style.cpp` → `Node::set_pointer_events`, read by `hit_test_deep`; `interaction_style` |
| `user-select` | applied | `interaction_style.cpp` → `Node::set_user_select`, read by `text_selection_allowed`; `interaction_style` |
| `visibility` | applied | `interaction_style.cpp` → `Node::set_style_hidden`; `Node::rendered()` then gates `paint_pass.cpp`, `hit_test`/`hit_test_deep`, `a11y_tree.cpp` and the focus walks. The layout box is kept, so unlike `display: none` the parent does not reflow. `property_effects` |
| `cursor` | applied | `cursor_value.cpp` is the shared keyword grammar; `interaction_style.cpp` → `Node::set_cursor` (inherited); the app maps the hovered node's kind with `app/window_cursor.cpp` `to_hal_cursor` and requests `hal::Window::set_cursor`, resolved by `src/platform/windows/window_cursor_win.*` (`WM_SETCURSOR`, stock cursors) and `src/platform/linux/window_cursor_x11.*` (`XCreateFontCursor`/`XDefineCursor`), reported as `WindowMetrics::cursor`. `url()` lists are `UnsupportedValue` and fall back to `auto`. Contract in [`window-chrome-v1.md`](window-chrome-v1.md) section 8.5; `window_cursor`, `window_cursor_fake`, `window_win32`, `window_x11` |

## 6. Scrolling

| Property | Status | Consumer / evidence |
|---|---|---|
| `scroll-behavior` | applied | `scroll_controller` smooth-scroll target; `scroll_properties` |
| `overscroll-behavior` | applied | `scroll_controller` `allow_chain`; `scroll_properties` |
| `overscroll-effect` | applied | `scroll_controller` effect request; `scroll_properties` |

## 7. Responsive and writing mode

| Property | Status | Consumer / evidence |
|---|---|---|
| `container-type` | applied | `scene_style.cpp` `AncestorContainers::push` decides which ancestors are query containers; `property_effects` |
| `container-name` | applied | `scene_style.cpp` matches a named `@container` prelude; `property_effects` |
| `writing-mode` | parsed only | V1 is horizontal-only: Yoga has no writing-mode input, the text seam lays out left-to-right lines, and [`text.md`](../design/text.md) section 12 records vertical layout as out of scope. The property is inherited and `@supports`-visible so a stylesheet written for both modes parses. Becomes **applied** when vertical writing mode lands. |

## 8. Animation and transition

The registry accepts these names so the cascade stores them and `@supports` answers true (issue
#250); their grammars and expansion live in `src/ui/animation/`, not in the registry.

`FramePipeline::sync_style` runs `StyleBridge` on every style stage, which is what makes these
names real (issue #271): the transition diff starts `start_style_transitions` for the properties the
computed style changed, and the animation half calls `sync_style_animations`. Both are evidenced by
`transition_wiring` end to end. The animation half needs the compiled sheet's `@keyframes` table:
`AppHost` installs it with `FramePipeline::set_style_keyframes` (`src/app/host.cpp`, at construction
and after every cascade rebuild, because the pipeline borrows the vector), so both halves run in the
app; a pipeline built without that call runs the transition half only, which is what the
`transition_wiring` animation case constructs on purpose. `docs/design/animation-runtime.md`
section 9 records the rules: which change starts a transition, what an interrupted transition does,
and the baseline/retention trade-offs.

| Property | Status | Consumer / evidence |
|---|---|---|
| `animation-name` | applied | `parse_style_animations` reads it and `sync_style_animations` starts the runtime animations; `FramePipeline::sync_style` drives that per node through `StyleBridge` (`style_bridge.cpp`) on every style stage; `animation_sync`, `transition_wiring` |
| `animation-duration` | applied | `parse_style_animations` → `parse_animation_longhands`; `FramePipeline::sync_style` → `StyleBridge`; `animation_sync`, `transition_wiring` |
| `animation-timing-function` | applied | `parse_style_animations` → `parse_animation_longhands`; `animation_sync`, `transition_wiring` |
| `animation-delay` | applied | `parse_style_animations` → `parse_animation_longhands`; `animation_sync`, `transition_wiring` |
| `animation-iteration-count` | applied | `parse_style_animations` → `parse_animation_longhands`; `animation_sync`, `transition_wiring` |
| `animation-direction` | applied | `parse_style_animations` → `parse_animation_longhands`; `animation_sync`, `transition_wiring` |
| `animation-fill-mode` | applied | `parse_style_animations` → `parse_animation_longhands`; `animation_sync`, `transition_wiring` |
| `animation-play-state` | applied | `parse_style_animations` → `parse_animation_longhands`; `ensure_style_animations` pauses/resumes the matching entries (reached through `StyleBridge`); `animation_sync`, `transition_wiring` |
| `animation` | applied | `parse_style_animations` falls back to the shorthand when `animation-name` is absent; `animation_shorthand`, `transition_wiring` |
| `transition-property` | applied | `parse_style_transitions` reads it (falling back to the shorthand) and `FramePipeline::sync_style` calls `start_style_transitions` per node when the computed value changes; `transition_wiring` |
| `transition-duration` | applied | `parse_style_transitions` → `parse_transition_longhands`; the diff starts an entry only for a non-zero duration; `transition_wiring` |
| `transition-timing-function` | applied | `parse_style_transitions` → `parse_transition_longhands`; `transition_wiring` |
| `transition-delay` | applied | `parse_style_transitions` → `parse_transition_longhands`; `transition_wiring` |
| `transition` | applied | `parse_style_transitions` falls back to `parse_transition_list` when `transition-property` is absent; `transition_wiring` |

## 9. What the cross-check does not cover

The test proves that the *set* of registered properties and this table agree, and that every
`applied` row names a test. It cannot prove that the named test still observes the effect: that is
what the tests themselves are for, and the ones added for issue #213 (`order`, `visibility`,
`text-overflow`, `container-type`/`container-name`, `font-style`) go through the real path rather
than asserting that a computed value exists.
