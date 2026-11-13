# MapView v1

Status: proposed (component library stage 6, [`component-library.md`](../design/component-library.md)
section 8; ships after V1). Builds on [`custom-elements-v1.md`](custom-elements-v1.md) (the map is a
custom element, not a core change), [`networking-v1.md`](networking-v1.md),
[`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md) and [`accessibility-v1.md`](accessibility-v1.md).

Terms: **tile** is a fixed-size square image of the map at one zoom level. **Slippy map** is the
`z/x/y` tile addressing (zoom, column, row) used by most tile servers. **Camera** is the visible
center, zoom and bearing. **Attribution** is the credit line a tile data license requires. See also
the [glossary](../reference/glossary.md).

## 1. Goals and non-goals

Goals:

- A pannable, zoomable 2D map with markers and shapes, drawn by the renderer with no embedded web
  view and no JavaScript.
- No hidden network use: the app names its tile source; there is no built-in provider.
- Bounded memory and network, cancellable, never blocking the UI thread.

Non-goals (not in this spec): turn-by-turn routing, geocoding and search, the device's location
(a platform service with its own permission flow), 3D terrain and buildings, vector tiles, offline
regions. Vector tiles and offline are deferred (decisions 1 and 2).

## 2. Component

`MapView` is a library component over a `MapSurface` custom element. The surface draws tiles and
shapes with the batched 2D renderer and the vector path rasterizer
([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md)); markers are drawn by the
surface from the `markers` prop, so a thousand markers do not become a thousand scene nodes.

## 3. Props

| Prop | Type | Default | Notes |
|---|---|---|---|
| `tileSource` | `{urlTemplate, tileSize, minZoom, maxZoom, attribution}` | required | `urlTemplate` uses `{z}`, `{x}`, `{y}`; `https` only |
| `center` / `defaultCenter` | `{lat, lng}` | `{0, 0}` | Controlled / uncontrolled pair |
| `zoom` / `defaultZoom` | number | `2` | Fractional, clamped to the source range |
| `bearing` | degrees | `0` | Rotation gesture is off unless `rotatable` |
| `rotatable` | bool | `false` | |
| `markers` | array of `{id, lat, lng, label, icon?}` | `[]` | `id` is the identity; `label` is the accessible name |
| `shapes` | array of `{id, kind: polyline or polygon, points, stroke, fill?}` | `[]` | Coordinates are WGS84 degrees |
| `label` | string (i18n key) | required | Accessible name of the map |

`attribution` is required text: the surface always draws it in a corner and an app cannot hide it
(tile data licenses require it). The style part can restyle its colors, not remove it.

## 4. Events

| Event | Payload | Rule |
|---|---|---|
| `onCameraChange` | `{center, zoom, bearing}` | Per frame while a gesture runs, at most once per frame |
| `onCameraIdle` | `{center, zoom, bearing}` | After a gesture or animation settles |
| `onMapPress` | `{lat, lng}` | A tap that did not hit a marker |
| `onMarkerPress` | `{id}` | |
| `onTileError` | `{z, x, y, error}` | At most one per tile; the map keeps working |

## 5. Tiles, network and memory

- Tiles are requested only for the visible region plus a one-tile margin. A tile that leaves the
  region before it loads is cancelled; the lower-zoom parent is drawn scaled until it arrives, so
  panning never shows holes.
- Requests go through the network stack, off the UI thread, with a bounded number in flight
  (default 6) and the response size limit of [`networking-v1.md`](networking-v1.md).
- The decoded-tile cache is bounded by bytes (default 64 MiB, configurable in runtime config) and
  evicts least recently used. Decoding happens at tile size, not larger.
- A tile server error never throws; the tile stays a placeholder and `onTileError` fires once.

## 6. Input and animation

- Pan (drag with fling), pinch zoom, double-tap zoom in, two-finger tap zoom out, mouse wheel
  zoom around the cursor, and rotate when `rotatable`, all through the normal gesture recognizers
  with capture/target/bubble routing. The map claims drag only after the slop distance so a
  parent scroll view still works around it.
- Programmatic camera moves (`center`/`zoom` prop changes) animate with the spring runtime unless
  reduced motion is set, in which case they jump.
- A static camera with all tiles loaded presents nothing (render-on-demand).

## 7. Accessibility

- The map has role `image` with `label` and is not itself a list of tiles.
- `markers` are exposed as a list: each marker is focusable with its `label`, Enter fires
  `onMarkerPress`, and focusing one scrolls it into view.
- Keyboard: arrows pan, `+` and `-` zoom, `0` resets bearing. Every pointer gesture has a key.
- Apps should also offer the same data as a normal `List`; the map is not the only way to reach it.

## 8. Styling parts

`surface`, `attribution`, `marker`, `marker-label`, `controls`, `zoom-in`, `zoom-out`,
`compass`. Part names are public API
([ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md)).

## 9. Testing

- Unit: tile coordinate math (lat/lng to tile and back, wrap at the date line, clamp at the poles),
  cache eviction, request cancellation, camera animation, marker hit-testing.
- Headless: a fake tile source returning a deterministic image per `z/x/y`; scripted pan, zoom,
  marker press; the semantics snapshot lists the markers.
- Golden: fake tiles plus markers and shapes at fixed cameras; default and dark themes.
- No test uses the network or a real tile provider.

## 10. Decisions

Decided by the owner on 2026-11-13:

1. **Raster tiles only** in the first release; vector tiles (MVT) and a style engine are a separate
   later spec.
2. **No offline tiles** in the first release. PMTiles (one file) is the first candidate later, subject
   to the license of any bundled data.
3. **No default tile provider.** The app always supplies `tileSource`. The policy of each public
   provider forbids uncontrolled production use, so the project does not choose one on the app's
   behalf.
4. **Attribution:** the app passes the attribution string; the framework always draws it and the
   app cannot hide it.
5. **Marker clustering** is the app's concern in V1.
