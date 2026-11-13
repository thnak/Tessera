# PdfViewer v1

Status: proposed (component library stage 6, [`component-library.md`](../design/component-library.md)
section 8; ships after V1). The export side of PDF (printing, writing PDF files) is in
[`media-and-export.md`](../design/media-and-export.md) section 2; this spec covers **viewing** only.
Builds on [`virtualization-v1.md`](virtualization-v1.md), [`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md),
[`resource-lifecycle-v1.md`](resource-lifecycle-v1.md) and [`accessibility-v1.md`](accessibility-v1.md).

Terms: **engine** is the library that parses a PDF and rasterizes its pages. **Page tile** is a
rendered rectangle of one page at one scale. **Text layer** is the page's text with positions, used
for selection and search. **Tagged PDF** is a PDF that carries a logical structure for assistive
technology. See also the [glossary](../reference/glossary.md).

## 1. Goals and non-goals

Goals:

- Show a PDF document with continuous scroll, zoom and page navigation, rendered natively.
- Treat every document as untrusted input: bounded memory, bounded time, no side effects.
- Rendering never runs on the UI thread; the viewer idles when nothing changes.

Non-goals (not in this spec): editing, form filling, annotations, digital signatures, embedded
JavaScript (never executed), attachments, printing from the viewer, and creating PDFs.

## 2. Engine seam

The viewer talks only to a `PdfEngine` interface, so the engine can differ per platform without
changing the component:

```cpp
class PdfEngine {
public:
    Task<Result<PdfDocument>> open(PdfSource, std::optional<std::string> password);
    Task<Result<PageInfo>> page_info(PdfDocument, int page);         // size in points, rotation
    Task<Result<Bitmap>> render(PdfDocument, int page, Rect region, float scale);
    Task<Result<TextLayer>> text(PdfDocument, int page);             // phase B
};
```

The choice of engine is an open decision (open question 1). A fake engine that returns
deterministic bitmaps backs all tests.

## 3. Component and props

`PdfViewer` is a library component. Page tiles are drawn as images; the page list is virtualized
like a `VirtualList`, so only visible pages (plus one on each side) hold tiles.

| Prop | Type | Default | Notes |
|---|---|---|---|
| `src` | asset id, file path, or bytes | required | URLs are the app's job: download first, then pass bytes or a file |
| `page` / `defaultPage` | 1-based int | `1` | Controlled / uncontrolled pair |
| `zoom` / `defaultZoom` | `fit-width`, `fit-page`, or number | `fit-width` | Number is a scale, clamped to `0.25..8` |
| `layout` | `continuous`, `single` | `continuous` | `spread` (two pages) is open question 4 |
| `password` | string | none | When absent and needed, `onPasswordRequired` fires |
| `label` | string (i18n key) | required | Accessible name |

## 4. Events

| Event | Payload | Rule |
|---|---|---|
| `onLoad` | `{pageCount, title?}` | Once per `src` |
| `onPageChange` | `{page}` | When the page nearest the viewport center changes |
| `onZoomChange` | `{zoom}` | |
| `onPasswordRequired` | `{attempt}` | The app supplies `password`; the viewer never shows its own dialog |
| `onLinkPress` | `{kind: internal or external, page?, uri?}` | Internal links also navigate; external links only report, never open by themselves |
| `onError` | `PdfError` | Section 7 |

## 5. Rendering, memory and threading

- Tiles are rendered at display size times device pixel ratio (the rule of issue #14 for
  images), in tiles no larger than 1024 by 1024 pixels, on a worker. Results return through the
  completion queue; a result for a page or scale that is no longer wanted is dropped.
- A lower-scale tile of the same page is drawn scaled until the sharp one arrives, so zooming never
  shows a blank page.
- The tile cache is bounded by bytes (default 128 MiB, configurable in runtime config) and evicts
  least recently used, never a tile currently on screen.
- A page that cannot be rendered shows an explicit placeholder with an error badge; other pages
  keep working.
- A static viewer presents nothing (render-on-demand); only tile arrival, scroll and zoom wake it.

## 6. Security

A PDF is attacker-controlled data. The viewer and every engine adapter must:

- never execute document JavaScript, launch files, or follow links automatically;
- enforce limits on file size, page count, page dimensions, decompressed stream size and per-page
  render time (defaults in runtime config); exceeding one is `limit_exceeded`, not a hang;
- run the engine where a crash or timeout cannot take down the UI thread; whether that means a
  separate process is part of open question 2;
- keep fonts embedded or pinned; no system font lookup for goldens.

## 7. Errors

`PdfError{code, message}` with codes `source_not_found`, `invalid_format`, `encrypted`,
`wrong_password`, `limit_exceeded`, `render_failed`, `engine_unavailable`. Errors never throw and
are shown as an explicit error state in the viewer.

## 8. Phases

| Phase | Adds |
|---|---|
| A | Open, render, scroll, zoom, page navigation, links, password |
| B | Text layer: selection and copy, search (`ref.find(query)`), outline (bookmarks) |
| C | Accessibility from tagged PDF |

Annotations and forms stay out of scope until a separate spec.

## 9. Accessibility, keyboard and parts

- Role `document` with `label`; each visible page is an `image` named "Page N of M" until phase C
  exposes real structure. A document without a text layer is reported as such, not hidden.
- Keyboard: Page Up/Down and arrows scroll, Home/End first and last page, `+`/`-` zoom, `0` fit.
  `onPageChange` announces the page politely.
- Parts: `viewport`, `page`, `page-label`, `placeholder`, `error`, `toolbar`, `zoom-in`,
  `zoom-out`, `page-input`
  ([ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md)).

## 10. Testing

- Unit: page layout and zoom math, virtualization window, tile cache eviction, cancellation of
  stale renders, password flow, limit handling against the fake engine.
- Headless: semantics snapshot with page images and the document role; scripted scroll, zoom,
  page jump.
- Golden: fake-engine pages (numbered test pattern) at fixed zooms; default and dark themes. A
  golden against the real engine is added only when the engine and its fonts are pinned.
- Fuzz-style tests feed truncated and corrupt files and assert an error, never a crash.

## 11. Open questions

1. **Engine.** Candidates are the platform renderers (Windows `PdfRenderer`, Apple PDFKit,
   Android `PdfRenderer`) and PDFium (BSD-3-Clause and Apache-2.0 parts, needs an entry in
   [`dependencies.md`](dependencies.md)). MuPDF is AGPL and excluded by the no-GPL rule. Linux has
   no platform renderer, so a bundled engine is needed there.
2. **Isolation:** in-process worker with limits, or a separate sandboxed process.
3. **Text layer and search** quality across languages and RTL text.
4. **Two-page spread** and reading direction.
5. **Wasm target:** whether the web build uses the same engine compiled to Wasm or the browser's
   own viewer.
