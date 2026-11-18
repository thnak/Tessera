// SPDX-License-Identifier: MIT

// The simulated device frame around the preview canvas (issue #171's skins slice;
// `designer-studio-v1.md` sections 5.1 and 5.2 are normative).
//
// **This is page chrome, not engine paint.** The engine draws exactly one thing, the preview canvas
// (ADR-054 decision 3), so the bezel, the bars and the cutout are HTML/CSS/SVG around and over it.
// That is also what makes the retint rule cheap: a theme switch changes CSS custom properties, the
// browser repaints the frame, and no engine frame is owed.
//
// **No colour is written here.** Every paint is `currentColor` or a `var(--skin-*)` token that
// `styles/skin.css` defines from the page's own theme tokens. Section 5.2: "Chrome colours are not
// part of the skin: the page tints the simulated bars from `theme.applied` ... A skin that
// hard-codes a colour is a bug." A literal colour in this file would be that bug in the one place
// the manifest validator cannot see it, which is why the check also scans the SVG frames.
//
// **The geometry is the manifest's, and only the manifest's.** Widths, heights and radii come from
// the validated `Skin` through `skin-geometry.ts`; nothing here invents a size. The canvas keeps its
// own constant attributes and the session sizes its backing store, so the frame can change without
// React touching the engine's surface.
//
// **The canvas is a child, and it is never remounted.** `children` is the canvas stack, rendered at
// a stable position in this tree: the frame's own props change when the skin does, but the canvas
// element's type and position do not, so React reconciles it in place. A `key` on the frame would
// break that, which is why there is none.

import type { ReactNode } from "react";
import { barsOf, cutoutRectOf, outerBoxOf, type GeometryMode } from "../views/skin-geometry";
import { skinArtUrl } from "../views/skin-load";
import type { Skin } from "../views/skins";

/** The status bar's abstract glyphs: a signal wedge, a wifi arc and a battery. No text, no colour. */
function StatusGlyphs() {
  return (
    <span className="skin-glyphs" aria-hidden="true">
      <svg viewBox="0 0 18 12" width="18" height="12">
        <path d="M0 12 L4 8 L4 12 Z M6 12 L10 6 L10 12 Z M12 12 L16 3 L16 12 Z" fill="currentColor" />
      </svg>
      <svg viewBox="0 0 16 12" width="16" height="12">
        <path d="M1 9 A10 10 0 0 1 15 9" fill="none" stroke="currentColor" strokeWidth="1.6" />
        <circle cx="8" cy="10.5" r="1.4" fill="currentColor" />
      </svg>
      <svg viewBox="0 0 24 12" width="24" height="12">
        <rect x="0.5" y="0.5" width="20" height="11" rx="2.5" fill="none" stroke="currentColor" />
        <rect x="2" y="2" width="14" height="8" rx="1.5" fill="currentColor" />
        <rect x="21.5" y="4" width="2" height="4" rx="1" fill="currentColor" />
      </svg>
    </span>
  );
}

/** The navigation bar: three buttons, or the gesture pill a modern phone shows instead. */
function NavigationGlyphs({ gesture }: { gesture: boolean }) {
  if (gesture) {
    return (
      <span className="skin-gesture" aria-hidden="true">
        <span className="skin-gesture-pill" />
      </span>
    );
  }
  return (
    <span className="skin-glyphs" aria-hidden="true">
      <svg viewBox="0 0 16 16" width="16" height="16">
        <path d="M10 3 L5 8 L10 13" fill="none" stroke="currentColor" strokeWidth="1.6" />
      </svg>
      <svg viewBox="0 0 16 16" width="16" height="16">
        <circle cx="8" cy="8" r="5" fill="none" stroke="currentColor" strokeWidth="1.6" />
      </svg>
      <svg viewBox="0 0 16 16" width="16" height="16">
        <rect x="3" y="3" width="10" height="10" rx="1.5" fill="none" stroke="currentColor" strokeWidth="1.6" />
      </svg>
    </span>
  );
}

/**
 * The frame around the preview canvas.
 *
 * `skin` is `null` before the manifests load and when every one of them was rejected; the frame then
 * renders the bare canvas, which is exactly the pre-#171 page. That is deliberate: a picker that
 * failed to load must not take the preview down with it.
 */
export function SkinFrame({
  skin,
  mode,
  children,
}: {
  skin: Skin | null;
  mode: GeometryMode;
  children: ReactNode;
}) {
  if (skin === null) {
    // The frame element exists either way, so `#skin-frame` is a stable contract: the page checks
    // assert it, and a session that wanted to measure the frame would find it before a skin loads.
    // `is-bare` is what makes it invisible - the pre-#171 page, with the canvas where it always was.
    return (
      <div className="skin-frame is-bare" id="skin-frame" data-skin="" data-mode={mode}>
        {children}
      </div>
    );
  }

  const bars = barsOf(skin, mode);
  const outer = outerBoxOf(skin, mode);
  const cutout = cutoutRectOf(skin);
  const art = skinArtUrl(skin);

  // The geometry travels as custom properties rather than as a style object per element: the sheet
  // owns the shapes, and a skin change is one attribute write instead of a dozen inline styles.
  const geometry = {
    "--skin-outer-width": `${outer.width}px`,
    "--skin-outer-height": `${outer.height}px`,
    "--skin-bezel-top": `${skin.bezel.top}px`,
    "--skin-bezel-right": `${skin.bezel.right}px`,
    "--skin-bezel-bottom": `${skin.bezel.bottom}px`,
    "--skin-bezel-left": `${skin.bezel.left}px`,
    "--skin-radius": `${skin.bezel.radius}px`,
    "--skin-viewer-width": `${skin.viewer.width}px`,
    "--skin-viewer-height": `${skin.viewer.height}px`,
    "--skin-title-height": `${bars.title}px`,
    "--skin-menu-height": `${bars.menu}px`,
    "--skin-status-height": `${bars.status}px`,
    "--skin-navigation-height": `${bars.navigation}px`,
  } as React.CSSProperties;

  return (
    <div
      className={`skin-frame mode-${mode}`}
      id="skin-frame"
      data-skin={skin.id}
      data-mode={mode}
      style={geometry}
    >
      {/* The strips the OS draws OUTSIDE the client area (section 5.1, `system` mode). They are
          siblings of the screen, so the canvas below them is the client area and the viewer reports
          zero insets. In `custom` mode both are zero-height: the engine draws the framework title
          bar, and drawing a second one would show two. */}
      {bars.menu > 0 ? (
        <div className="skin-menu" aria-hidden="true">
          <span className="skin-menu-mark" />
          <span className="skin-menu-mark" />
          <span className="skin-menu-mark" />
        </div>
      ) : null}
      {bars.title > 0 ? (
        <div className="skin-title" aria-hidden="true">
          <span className="skin-title-dots">
            <span />
            <span />
            <span />
          </span>
          <span className="skin-title-label">{skin.name}</span>
        </div>
      ) : null}

      <div className="skin-screen">
        {children}

        {/* The overlaid bars (`mobile` mode, section 5.1: "the skin overlays the status bar,
            navigation bar and cutout on top of the canvas"). They are `pointer-events: none` so the
            preview's own pointer path keeps working underneath them, exactly like the picker's
            overlay. */}
        {bars.status > 0 ? (
          <div className="skin-status" aria-hidden="true">
            <span className="skin-status-time" />
            <StatusGlyphs />
          </div>
        ) : null}
        {bars.navigation > 0 ? (
          <div className="skin-navigation" aria-hidden="true">
            <NavigationGlyphs gesture={bars.gesture} />
          </div>
        ) : null}

        {/* The cutout, in viewer coordinates. It is a hole in the display, so it is drawn in the
            frame's own colour over the canvas rather than as a tint. */}
        {cutout !== null ? (
          <div
            className={`skin-cutout is-${skin.cutout.type}`}
            aria-hidden="true"
            style={{
              left: `${cutout.x}px`,
              top: `${cutout.y}px`,
              width: `${cutout.width}px`,
              height: `${cutout.height}px`,
            }}
          />
        ) : null}
      </div>

      {/* The bezel and the optional SVG frame. The art is a CSS mask rather than an `<img>`, so it
          is painted in `currentColor` and follows a theme switch; an `<img>` would keep whatever
          colour the file was saved with, which is the retint bug section 5.2 names. */}
      <div className="skin-bezel" aria-hidden="true" />
      {art !== null ? (
        <div
          className="skin-art"
          aria-hidden="true"
          style={{ maskImage: `url("${art}")`, WebkitMaskImage: `url("${art}")` }}
        />
      ) : null}
    </div>
  );
}
