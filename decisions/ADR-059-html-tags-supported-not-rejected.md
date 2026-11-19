# ADR-059: HTML tags are supported or approximated, not rejected

- Status: Accepted (2026-11-19, owner). Amends [ADR-039](ADR-039-lowercase-html-tags-accepted.md)
  decisions 3 and 4 and two of its rejected alternatives ("Map every HTML tag", "Add UA default
  styles"). The owner asked for this direction and answered the four questions on 2026-11-19 (see
  "Owner answers"). ADR-039 stays in force for everything it already maps.
- Date: 2026-11-19
- Related: [ADR-039](ADR-039-lowercase-html-tags-accepted.md) (lowercase tags are aliases),
  [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own front end, React-like),
  [ADR-058](ADR-058-react-vite-port-compatibility-tiers.md) (port classes, `port-check`),
  [`html-aliases.md`](../docs/specs/jsx-intrinsics-v1/html-aliases.md) (the normative tag table),
  [`stylesheet-property-status-v1.md`](../docs/specs/stylesheet-property-status-v1.md),
  [`react-compat.md`](../docs/specs/jsx-intrinsics-v1/react-compat.md)

## Context

ADR-039 accepted the lowercase tags that map cleanly (`div`, `span`, `h1`, `button`, `input`, ...)
and kept `TESSERA_JSX_SUBSET_HTML_TAG` for the rest. It rejected "map every HTML tag" because a tag
with no true equivalent (`table`, `select`, `form`, `video`) "would compile into something that
behaves differently from the browser", and it rejected default styles per alias for V1.

That was a sound call for a first release, but it is a statement about what Tessera does not do
yet, not about what it cannot do. The goal in ADR-058 is that an existing React + Vite project ports
with small edits. Every tag that is an error is a rewrite of every page that uses it; a table-heavy
or form-heavy project is blocked entirely. Checking the engine as it stands (2026-11-19):

- Almost every rejected tag is a **missing capability**, not an impossibility: tables need a layout
  mode, `u`/`s`/`sub`/`sup` need text properties, `form`/`select`/`checkbox` need library controls.
- Some capabilities are visibly absent from the property registry: `text-decoration`,
  `vertical-align`, `list-style` and `outline` have no row in `stylesheet-property-status-v1.md`
  (they are not registered), and `display` maps only `none`, `contents`, `flex` and `block`. Grid
  tracks and placement properties do exist.
- Only a small group is impossible without a web engine or a JS runtime: `iframe`, `embed`,
  `object`, `script` and `dangerouslySetInnerHTML` (ADR-024, ADR-036 item 6).

Two things from ADR-039 stay right and are kept: the front end resolves tags in analysis, the AST
keeps the tag as written, and the IR keeps its **closed** list of node kinds. A tag is a spelling;
`View` and `Text` are the carriers (`ir-v1.md` section 3).

## Decision

### 1. Every HTML tag has one of four statuses, and "rejected" is rare

The tag table in `html-aliases.md` gains a status per tag. A tag is never silently dropped.

| Status | Meaning | Diagnostic |
|---|---|---|
| **mapped** | Same meaning as in the browser for the attributes it accepts (today's rows) | none |
| **approximated** | Accepted; one or more documented deviations from the browser that cannot be closed (font metrics, browser-only behavior), listed in the row. **Never used for unfinished work** | `TESSERA_JSX_HTML_APPROX` (warning by default, error when `ui.strict` is set) naming the deviation |
| **planned** | Not yet implemented; the row names the issue and the capability it waits for. A tag stays `planned` until it is implemented completely | `TESSERA_JSX_SUBSET_HTML_TAG` with the issue number, as today |
| **rejected** | No meaningful behavior without a web engine or JS runtime, with the reason | `TESSERA_JSX_SUBSET_HTML_TAG` with the reason |

`rejected` is limited to `iframe`, `embed`, `object`, `script`, `dangerouslySetInnerHTML` and
attributes that name a browser-only concept (`contentEditable`, `srcDoc`). Everything else moves to
`mapped`, `approximated` or `planned`, and the table shows which. Tags that exist only inside a web
document (`html`, `head`, `body`, `meta`, `link`, `base`) are accepted with a warning and have no
effect when written inside a component (React 19 hoisting); `title` will set the window title when
that setting exists. `style` containing CSS text is `planned`, tied to the stylesheet engine.

### 2. Carriers stay `View` and `Text`; meaning moves to role, reserved class and props

The IR does not gain a node kind per tag. A tag resolves, as in ADR-039, to its carrier. What a tag
means beyond the carrier is carried by three existing mechanisms:

1. the injected `role` (already used for `ul`, `li`, `h1`-`h6`, `a`), extended to `table`, `row`,
   `cell`, `columnheader`, `dialog`, `group` and the others the accessibility role list has;
2. a **reserved class** the analysis adds to the node, named after the tag as written
   (`.tsr-h1`, `.tsr-table`; the prefix is reserved and an author class may not use it), so a
   stylesheet can target a tag the way a browser's user-agent sheet does;
3. new props on the carrier when behavior needs them (for example `href` on a `Text` that is a link,
   resolved through navigation, section 5).

If a tag cannot be carried this way without distorting a carrier (tables are the likely case), the
ADR that adds it may add a node kind; the rule is only that it needs a decision, not a diagnostic.

### 3. A browser default stylesheet, on by default (resolves Q12)

`tessera.json` gets `ui.htmlDefaults`: `"browser"` or `"none"`. The owner chose `"browser"` as the
default (2026-11-19): a new project, and every project made by a port command or a template, gets
`"browser"`. A `tessera.json` that predates this key and omits it keeps `"none"`, so no existing
project or golden changes until its author turns it on. With `"browser"` the compiler adds a bundled stylesheet at the lowest precedence, below
every author rule and above the engine defaults, that gives the mapped tags the browser's
user-agent look through the reserved classes of section 2: heading sizes and weights, paragraph and
list margins and markers, `strong`/`em` weights, link color and underline, form control spacing.
Author CSS and Tailwind-generated CSS always win. A project written for Tessera that wants the
bare engine look sets `"none"`. The stylesheet is data, versioned with the tag table, and has its
own goldens. It does not claim pixel parity with any browser (ADR-036 item 2).

### 4. Capabilities the engine needs, by group

Each group is one issue family. A tag moves from `planned` to `approximated` or `mapped` when its
group lands; the table, the typings and the tests change together (ADR-039 consequence).

| Group | Tags | Needs |
|---|---|---|
| **A. Text and breaks** | `br`, `hr`, `pre`, `blockquote`, `q`, `cite`, `u`, `s`, `small`, `mark`, `kbd`, `sub`, `sup`, `abbr` | `text-decoration`, `vertical-align`/baseline shift, `list-style`, `white-space: pre` (exists), line-break node in text runs |
| **B. Links and labels** | `a href`, `label htmlFor` | `href` implemented in both forms in the first slice: an internal path resolves through the navigation service and its route table, an external URL opens through the shell service; label-to-control focus and activation |
| **C. Browser look** | all mapped tags | the stylesheet of section 3 |
| **D. Tables** | `table`, `thead`, `tbody`, `tfoot`, `tr`, `td`, `th`, `caption`, `colgroup`, `col` | a complete table layout (content-driven column sizing, `colspan`/`rowspan`, captions, header and footer groups, `border-collapse`) built on the grid machinery, or a node kind (section 2). **No approximation first**: the tags stay `planned` until the whole layout is implemented, by the owner's rule against patches that stretch over time |
| **E. Forms and controls** | `form`, `select`, `option`, `optgroup`, `datalist`, `input` `checkbox`/`radio`/`range`/`date`/`time`/`file`, `progress`, `meter`, `dialog`, `details`, `summary` | library `Checkbox`, `Radio`, `Slider`, `Select`, `Progress`, overlay-backed `dialog`, and a form controller behind `form`/`onSubmit` |
| **F. Media and graphics** | `svg` family, `canvas`, `video`, `audio`, `picture`, `source`, `track` | `svg` onto `Path` and the vector layer (ADR-057); `canvas`, `video`, `audio` are separate engine work and stay `planned` until a media ADR exists |

### 5. Compatibility is reported, not hidden

`tessera port-check` (ADR-058) prints each tag with its status and the issue it waits for, so a
port knows before it starts which pages are blocked and by what.

## Consequences

- A port stops failing on `table`, `br`, `form`, `select` and the other tags: it compiles with
  warnings that list the deviations, and the list shrinks as groups land.
- The tag table becomes a roadmap as well as a mapping; it must stay accurate (a row says `planned`
  only with a real issue).
- `approximated` can mislead: a page compiles and looks different. Mitigation: the warning names the
  deviation, `ui.strict` turns it into an error, and `port-check` lists them.
- The reserved class and the bundled stylesheet add a small, versioned surface to maintain, and the
  `tsr-` prefix must be reserved in the stylesheet engine and in `*.module.css` scoping.
- Several groups need engine properties that do not exist yet (`text-decoration`,
  `vertical-align`, `list-style`, table layout). Their cost is real work in the text and layout
  engines, not in the front end; this ADR commits to the work, not to a date.
- ADR-039 decision 4 ("an alias adds no default styles") is narrowed: it still holds when
  `ui.htmlDefaults` is `"none"`.

## Alternatives

- **Keep rejecting unmapped tags (ADR-039 as written).** Rejected: the cost lands on every port and
  the reason given was a missing capability, not an impossibility.
- **Map every tag silently.** Rejected: a table that looks wrong with no message is worse than an
  error; the warning and the status column are what make approximation honest.
- **One IR node kind per HTML tag.** Rejected: it grows the IR (and preview, codegen, goldens) by
  roughly a hundred kinds for a naming convenience; section 2's carriers plus role and class cover
  the meaning, and a real new kind stays possible where a carrier would be distorted.
- **Turn the browser default stylesheet on for every project.** Rejected for now: it changes the look
  of existing Tessera projects and their goldens; it is one setting for a port to turn on.
- **Embed a web view for the hard tags.** Rejected: it breaks ADR-024 and the deterministic golden
  model.

## Owner answers (2026-11-19)

1. **Browser defaults:** `ui.htmlDefaults` defaults to `"browser"` (section 3). Reading to confirm: a
   `tessera.json` without the key stays `"none"` so existing projects do not change.
2. **Severity of `approximated`:** a warning by default and an error when `ui.strict` is set (the
   proposal, accepted as written). It decides whether a page that compiles with a documented browser
   deviation fails the build. With the narrowed definition in section 1 (only deviations that cannot
   be closed, never unfinished work) it fires rarely, and CI can opt into the error.
3. **Tables: implement completely**, no approximate first slice (group D).
4. **`a href`: both** internal navigation and external URLs in the first slice (group B).

## Follow-up

- Add the status column and the four statuses to `html-aliases.md`; generate the typings from it.
- Open one issue per group A-F under milestone V1-M5; group C and the missing properties of
  group A are the first, since the other groups build on them.
- ADR-039 is marked amended by this ADR (its status line links here); its decisions 3 and 4 are
  read through section 1 above.
- Document `ui.htmlDefaults` and `ui.strict` in `project-config-v1.md` with the first slice that
  implements them.
