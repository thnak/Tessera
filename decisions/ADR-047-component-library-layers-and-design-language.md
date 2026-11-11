# ADR-047: Component library layers, theme versioning, design language and coverage

- Status: Accepted (2026-11-11)
- Date: 2026-11-11
- Amends: [ADR-032](ADR-032-bundled-component-library.md) items 1, 2, 3 and 6, and the catalog and
  roadmap in [`component-library.md`](../docs/design/component-library.md) sections 3 and 8.
- Related: [ADR-004](ADR-004-buy-vector-path-rasterization.md) (vector paths),
  [ADR-014](ADR-014-project-config-format.md) (project config),
  [ADR-016](ADR-016-stylesheet-engine.md) (stylesheet engine),
  [ADR-034](ADR-034-interaction-and-overscroll-animation.md) (platform interaction profiles),
  [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own front end),
  [ADR-037](ADR-037-self-contained-wasm-designer.md) (designer picker),
  [ADR-046](ADR-046-web-target-for-demos.md) (web target), issue
  [#70](https://github.com/thnak/Tessera/issues/70).
- Terms: **behavior primitive** = a component or hook that owns state, keyboard, focus and
  accessibility for a control but draws nothing of its own. **Styled component** = a primitive
  plus default structure and styles from design tokens. **Part** = a named inner element of a
  styled component that stylesheets can target. **Eject** = copying a component's source into the
  application so the application owns it.

## Context

A framework that ships only primitives asks every team to build buttons, dialogs and data grids
before their first screen. A complete, good-looking first-party library is one of the main reasons
people pick Flutter, WPF or Avalonia. ADR-032 already commits to a separate, accessible,
token-themed `tessera-ui` package. Experience with other first-party libraries shows four gaps
that ADR-032 does not close:

1. **Library tied to the core or the OS.** SwiftUI controls follow OS releases; WinUI 3 has had no
   first-party DataGrid since the Community Toolkit one was retired; Flutter's Material and
   Cupertino libraries lived inside the framework for years. In Tessera the two existing library
   components (`Field`, `Form`) are hand-built IR inside the compiler, with magic ids and spans to
   files that do not exist (issue #70), so the code already drifts toward this problem.
2. **Visual changes on upgrade.** Moving from Material 2 to Material 3 changed the look of many
   Flutter apps on a framework upgrade.
3. **A customization cliff.** Colors are easy to change; structure is not. Flutter widgets are
   copied to change them, and WPF and Qt Quick Controls need a full template rewrite. The React
   ecosystem solved this by separating behavior from looks (headless libraries such as Radix and
   React Aria) and by letting apps copy a component's source. ADR-032 treats headless use as an
   escape hatch rather than the foundation.
4. **Mobile-first coverage.** Material-style libraries miss dense desktop and tool controls
   (number input, splitter, menu bar, property grid, docking), so desktop teams buy third-party
   suites. Charts are often the first request for dashboard-style apps, yet ADR-032 puts them
   last.

The library also needs a recognizable look of its own. Imitating each platform's native look
(Flutter Cupertino, Qt styles) always lags the platform and never matches it.

## Decision

1. **The library is ordinary Tessera source.** `tessera-ui` is written in TSX and CSS and compiled
   by the same front end, IR and codegen as application code. No library component is built as IR
   inside the compiler. This needs bare package specifiers (`from "tessera-ui"`) resolved through
   the project config (ADR-014); [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md) section 4
   is extended accordingly. `Field` and `Form` move to the package (issue #70).
2. **Three layers, all public.**
   - `tessera-ui/primitives`: behavior primitives (press, toggle, focus scope, roving focus, list
     and grid navigation, dismissable layer, typeahead, slider value, text-field state). They own
     the accessibility contract of ADR-032 item 4 and draw nothing. Whether a primitive is a
     component or a hook follows what the JSX subset supports when it is built.
   - `tessera-ui`: styled components built only from primitives, tokens and parts.
   - **Eject**: `tessera ui add <Component>` copies a styled component's source into the project
     with a version stamp. The copy is the application's code from then on.
3. **Parts are the styling API.** Each styled component names its inner elements (for example
   `track`, `thumb`, `label`) and the stylesheet engine gains a `::part(name)` pseudo-element to
   target them. Part names are public API under SemVer; restyling a component never needs a
   template rewrite.
4. **Themes are versioned separately from components.** A theme is a token set plus part styles
   with its own version. Applications pin a theme version. Within one theme major version, default
   visuals change only to fix bugs, each with reviewed goldens; a new look ships as a new theme
   major version or a new theme. Component releases never change the look of a pinned theme.
5. **Default design language: "Instrument".** Tessera's default theme takes its own identity from
   technical and industrial instrument design (lab equipment, synthesizers, engineering
   drawings) instead of imitating any platform:
   - a strict 4 px base grid, straight lines, 1 px hairline borders, visible structure through
     dividers and small uppercase labels;
   - small, precise corner radii (the token scale stays in the 2 to 8 px range; no pill shapes
     except where a control's meaning needs one, such as a switch track);
   - flat surfaces: elevation by border and surface tone, with shadows only for floating layers;
   - neutral greys plus one saturated accent per theme, used for state and focus;
   - a grotesque sans for text and a monospace face for values and labels, with tabular numbers
     everywhere numbers change;
   - short, mechanical motion (ease-out, no overshoot by default) that respects reduced motion;
   - light, dark and high-contrast variants from day one, all meeting WCAG 2.2 AA contrast for
     text; the accent is used for fills and indicators, never as the only carrier of small text.
   The theme is original work: it uses no third-party trademarks, logos, product layouts or
   proprietary typefaces. Fonts are open-licensed (OFL-1.1) and recorded in
   [`dependencies.md`](../docs/specs/dependencies.md). Other themes can be built on the same
   tokens and parts.
6. **Platform behavior, not platform looks.** Components follow the active platform profile
   (ADR-034) for behavior: dialog button order, shortcuts, menu bar placement on macOS, scroll and
   overscroll physics, focus-ring visibility rules. The visual theme stays the same across
   platforms.
7. **Full coverage in stages.** The catalog covers desktop, data, media and motion components, not
   only mobile basics: among others NumberInput, MenuBar, Toolbar, StatusBar, Splitter,
   PropertyGrid, DataGrid, Spreadsheet, charts from sparklines to real-time streaming, Image,
   Video, Lottie, AnimatedIcon, SafeArea for notches/cutouts/folds, LiveCapsule (a floating
   status capsule around the top cutout), and ViewTransition and
   SharedElement on ADR-035. The catalog and stages live in
   [`component-library.md`](../docs/design/component-library.md) sections 3 and 8, the single
   source for them. Staging rules:
   - stages follow dependencies: behavior primitives and package imports (stage 0) come before any
     styled component, overlays before menus and pickers, `VirtualGrid` before DataGrid and
     Spreadsheet, media services before Video;
   - each stage delivers something buildable: settings screen (1), multi-screen app (2), CRUD and
     simple dashboard (3), desktop tools and monitoring (4), media/document apps (5), specialized
     integrations (6);
   - basic charts come in stage 3, not last, because dashboards are a common first use;
   - a stage closes only when its components meet item 10 and its starter template builds.
8. **Icons from Iconify, imported at build time.** Icons come from pinned versions of the
   [Iconify](https://iconify.design/) icon sets through a `tessera icons add <set>:<name>` tool,
   never from the Iconify API at run time. A license gate reads each set's metadata: MIT, ISC,
   Apache-2.0, BSD, CC0 and OFL-1.1 pass, CC-BY passes with generated attribution, share-alike
   needs an explicit opt-in, and GPL-family, non-commercial and unknown licenses are rejected.
   Static icons become vector paths (ADR-004) and are tree-shaken. Animated icons convert a
   defined subset of SVG SMIL and CSS `@keyframes` to Tessera keyframes at build time and fall back
   to a static frame with a diagnostic for anything outside it; they respect reduced motion and
   render-on-demand. Lottie is a separate stage-5 component.
9. **Gallery and starters are part of the library.** `samples/gallery` shows every component in
   every state with theme, density, RTL and text-scale switches, runs on native and on the web
   target (ADR-046), and is published by CI. Starter templates (dashboard, settings, list-detail
   CRUD) are built only from public library components.
10. **Definition of done per component.** ADR-032 item 4 and item 5 apply, plus: a primitive with a
    headless interaction test, a gallery page, documented parts, and goldens for each theme
    variant.

## Consequences

### Positive

- The library dogfoods the full pipeline (front end, rules, IR, codegen, designer picks), so
  pipeline gaps show up in first-party code before users meet them.
- Apps can restyle with tokens, then parts, then eject, without a cliff between the steps.
- Upgrading Tessera never changes an app's look unless it changes its pinned theme.
- A distinctive default look and a web-hosted gallery make the framework easy to evaluate.
- Desktop and dashboard users get the controls they need without a third-party suite.

### Negative / risks

- Package resolution and `::part` are new compiler and stylesheet work before stage 1 can finish.
- Three public layers and part names are a larger SemVer surface to maintain.
- Ejected components do not receive library fixes automatically; the version stamp only lets
  tooling report that an update exists.
- A strong default identity will not fit every product; other themes on the same tokens are the
  answer, and they cost design time.
- Stage 3 charts add scope earlier than ADR-032 planned; the full catalog is large and stages 5
  and 6 depend on platform media services and optional packages.
- Iconify sets mix licenses; the gate must stay strict so one icon cannot pull an incompatible
  license into an app.

## Alternatives considered

- **Keep components as compiler-built IR.** Rejected: see issue #70 and Context item 1.
- **Styled components only, with template overrides.** Rejected: reproduces the WPF/Qt
  customization cliff.
- **Headless primitives only, no default look.** Rejected: the goal is a framework people can
  build with on day one.
- **Native look per platform.** Rejected: always lags and never matches; behavior profiles give
  the parts of "native feel" that users notice most.
- **Adopt an existing public design system (Material, Fluent) as the default.** Rejected: ties
  Tessera's look to another vendor's roadmap and licensing terms and gives no identity of its own;
  such a system can still be offered later as an alternative theme where its license allows.

## Follow-up

- Bare package specifiers in `jsx-subset-v1.md` and the front end; project config entry for
  packages.
- Migrate `Field` and `Form` to TSX (issue #70).
- `::part(name)` in `stylesheet-v1.md` and the stylesheet engine.
- Instrument token set (JSON v1, G-25) with light, dark and high-contrast variants; font choice
  and license entries.
- Iconify import tool with the license gate, vector-path conversion and the SMIL/keyframe
  converter for animated icons.
- `samples/gallery` with a CI web build ([ADR-046](ADR-046-web-target-for-demos.md)).
