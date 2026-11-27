// SPDX-License-Identifier: MIT

// The preview's fold-hinge and safe-area overlay (issue #433), pure: the report the engine wrote,
// turned into the boxes the page draws over the canvas.
//
// **Why the geometry is not computed here.** The band a `book` fold occupies is a layout input, and
// the engine resolved it from the posture (`hal::fold_info_for`) before it laid the tree out. A page
// that recomputed it would be a second mapping - one that can disagree with the layout the overlay
// exists to explain, which is the one thing an overlay must never do. So `_tessera_canvas_overlay`
// reports the `WindowMetrics` the live tree was laid out against, and this module only scales those
// logical rectangles into the overlay's CSS pixels.
//
// **Why it is a separate module from the DOM half.** The scaling rule is the only part that can be
// wrong in an interesting way (a zoomed pane, a DPR, a canvas whose CSS box is narrower than its
// logical width), and it is a pure function of two numbers. `canvas-fold-layer.ts` owns the DOM;
// `scripts/ci/preview_fold_overlay.mjs` gates this half without a browser.

/** What `_tessera_canvas_overlay` reports; the shape `canvas_overlay_report_json()` writes. */
export interface CanvasOverlayReport {
  width: number;
  height: number;
  foldState: "none" | "flat" | "half-opened";
  foldOrientation: "none" | "horizontal" | "vertical";
  foldOcclusion: "none" | "full";
  separating: boolean;
  /** Degrees, or -1 for "nothing measured one" (which a simulated posture always is). */
  hingeAngle: number;
  hinge: Box;
  insets: Insets;
  ime: Insets;
  gestures: Insets;
  cutouts: Box[];
}

export interface Box {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface Insets {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

/** One rectangle the overlay draws, in the overlay's own CSS pixels. */
export interface OverlayBox {
  /** The class the layer sets, so the CSS picks the look: `fold-overlay-hinge`, `-inset`, `-cutout`. */
  kind: "hinge" | "inset" | "cutout";
  left: number;
  top: number;
  width: number;
  height: number;
}

/**
 * The inset a SafeArea pads by, per side: the largest of the three inset sources.
 *
 * `hal::resolve_safe_area` raises the padding from the sources it is given by taking the maximum per
 * side, because they overlap (an IME inset already includes the bars under it). This is the same
 * rule for the same reason - the overlay shades the space the content was pushed out of, and two
 * overlapping bands staggered on top of each other would show a seam that is not in the layout.
 */
export function resolvedInsets(report: CanvasOverlayReport): Insets {
  return {
    left: Math.max(report.insets.left, report.ime.left, report.gestures.left),
    top: Math.max(report.insets.top, report.ime.top, report.gestures.top),
    right: Math.max(report.insets.right, report.ime.right, report.gestures.right),
    bottom: Math.max(report.insets.bottom, report.ime.bottom, report.gestures.bottom),
  };
}

/** True when a box has area. A zero-area band is "nothing to draw", never a hairline. */
function hasArea(box: Box): boolean {
  return box.width > 0 && box.height > 0;
}

/**
 * The boxes the overlay draws, in the overlay's CSS pixels.
 *
 * `cssPerLogical` is the overlay's size over the report's logical size, so one logical pixel of the
 * layout becomes the number of CSS pixels it occupies on screen. It is the same factor the picker
 * uses (`canvas-picker.ts`'s `canvasMapping`), and for the same reason: the canvas' CSS box is the
 * logical viewport at 100% and differs only when a narrow pane shrinks it, so a DPR-based conversion
 * would put every box in the wrong place.
 *
 * Nothing is drawn for a report with no fold and no insets: a flat foldable and an inset-free desktop
 * window both produce an empty list, so the overlay element stays hidden and the picture is what it
 * was before the toggle existed.
 */
export function overlayBoxes(
  report: CanvasOverlayReport,
  cssPerLogical: number,
): OverlayBox[] {
  const scale =
    Number.isFinite(cssPerLogical) && cssPerLogical > 0 ? cssPerLogical : 1;
  const width = report.width * scale;
  const height = report.height * scale;
  const boxes: OverlayBox[] = [];

  // The hinge first, so it paints over the inset bands it sits inside: a book fold's band runs the
  // full height, through the top and bottom insets.
  if (hasArea(report.hinge)) {
    boxes.push({
      kind: "hinge",
      left: report.hinge.x * scale,
      top: report.hinge.y * scale,
      width: report.hinge.width * scale,
      height: report.hinge.height * scale,
    });
  }

  const insets = resolvedInsets(report);
  // One inset band per side, each spanning the full extent of the side it eats into. Drawn as four
  // bands rather than a frame so the middle of the window - the part the content keeps - is never
  // shaded.
  if (insets.top > 0) {
    boxes.push({ kind: "inset", left: 0, top: 0, width, height: insets.top * scale });
  }
  if (insets.bottom > 0) {
    boxes.push({
      kind: "inset",
      left: 0,
      top: height - insets.bottom * scale,
      width,
      height: insets.bottom * scale,
    });
  }
  if (insets.left > 0) {
    boxes.push({ kind: "inset", left: 0, top: 0, width: insets.left * scale, height });
  }
  if (insets.right > 0) {
    boxes.push({
      kind: "inset",
      left: width - insets.right * scale,
      top: 0,
      width: insets.right * scale,
      height,
    });
  }

  for (const cutout of report.cutouts) {
    if (!hasArea(cutout)) {
      continue;
    }
    boxes.push({
      kind: "cutout",
      left: cutout.x * scale,
      top: cutout.y * scale,
      width: cutout.width * scale,
      height: cutout.height * scale,
    });
  }
  return boxes;
}

/**
 * The overlay's caption: what the picture is showing, in one line.
 *
 * A posture the person can read back is what makes the overlay a debugging aid rather than
 * decoration - "book, hinge 24 px" says which device the layout answered for. The angle is named
 * only when something measured one: a simulated posture reports -1, and `-1°` would be a number a
 * developer might believe.
 */
export function overlayCaption(report: CanvasOverlayReport): string {
  if (report.foldState === "none") {
    return "no fold";
  }
  if (report.foldState === "flat") {
    return "flat";
  }
  const axis = report.foldOrientation === "vertical" ? "vertical hinge" : "horizontal hinge";
  const thickness = Math.round(
    report.foldOrientation === "vertical" ? report.hinge.width : report.hinge.height,
  );
  const angle =
    report.hingeAngle >= 0 ? `, ${Math.round(report.hingeAngle)}°` : "";
  return `${axis} ${thickness}px${angle}`;
}
