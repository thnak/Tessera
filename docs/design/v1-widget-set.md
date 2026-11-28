# V1 widget set (decision note for #527)

Status: **proposed, owner-gated.** [#527](https://github.com/thnak/Tessera/issues/527) asks what the
V1 widget set concretely is. The facts it rests on are decided and are not reopened here: stages 0
and 1 of [`component-library.md`](component-library.md) section 8 ship in V1 ([ADR-032
amendment](../../decisions/ADR-032-bundled-component-library.md), 2026-11-13), the library is
ordinary TSX in layers ([ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md)),
and the web gallery (#290) is in V1 (2026-11-18, [library-farm-plan.md](library-farm-plan.md)
section 6). This note records the recommended answer, the order the farm builds it in, and the
exact points only the owner can settle. The catalog stays normative in `component-library.md`.

## 1. Recommendation

The issue's suggested default stands: **option C with B's cut as the fallback.** The set is whatever
the Settings starter template consumes, and the template is built first. If it needs more than
about 20 components, the set falls back to the named list in section 2 and the remainder moves to
V1.1. The reasons are the issue's: breadth without a maintained core is the failure mode the
comparison set shows, and one screen a stranger can build is the evidence the architecture needs.

## 2. The named V1 set (fallback list, 23 components)

| Group | V1 | Status (2026-11-28) |
|---|---|---|
| Foundations | Text, Heading, Divider, Spacer, VisuallyHidden | not built (Text is an intrinsic today) |
| Actions | Button, IconButton, Link | Button done; the rest not built |
| Inputs | TextInput, TextArea, Checkbox, Switch, Select | **Checkbox and Switch built**; TextInput/TextArea not built (primitives exist); Select blocked (section 4) |
| Layout | Stack, Card, ScrollView | not built |
| Feedback | Spinner, Progress, Badge, EmptyState | not built |
| Overlay | Tooltip | not built (the overlay host landed in #302) |
| Media / data | Image, List | not built |

Already carried by earlier work and not counted above: Field, Form, SafeArea, Icon (stage 0).

**Moves to V1.1 under the fallback (15):** SegmentedControl, PasswordInput, SearchInput,
NumberInput, RadioGroup, Slider, Grid, AspectRatio, Skeleton, Chip, Banner, Avatar, Kbd, Code,
Label.

**Built ahead of the decision (no harm if the cut is taken):** `Radio` and `RadioGroup` landed with
Switch and Checkbox because they share the controlled-input shape and one gallery page. Under the
fallback they ship as available components that are not part of the V1 exit gate.

## 3. Build order (farm)

Ordered by dependency, within [library-farm-plan.md](library-farm-plan.md) phases:

1. Controlled toggles: Switch, Checkbox, RadioGroup (**done**, this slice).
2. Foundations and layout: Text, Heading, Divider, Spacer, VisuallyHidden, Stack, Card, ScrollView.
3. Actions: IconButton, Link. Inputs on the existing primitives: TextInput, TextArea.
4. Feedback: Spinner, Progress, Badge, EmptyState. Media and data: Image, List. Tooltip.
5. Select, once its popup is decided (section 4).
6. The Settings starter template (**started**: [`samples/settings`](../../samples/settings/README.md), `gallery --page settings`, goldens `settings_*`; its gap list is the README's), assembled from the above (option C's artifact). It is built
   incrementally as soon as step 2 exists, so the set is measured, not guessed.

Each component lands as one component per issue (a phase 3 input per PR), with the ADR-047 item 10
definition of done and the parity page from the farm plan.

## 4. Owner-gated items

| # | Question | Why it is not decided here |
|---|---|---|
| 1 | Option C (template defines the set) or option B (the 23-component list) as the V1 contract | The issue asks the owner; scope changes of V1 are an ADR amendment, not a silent spec edit. |
| 2 | The ADR-032 amendment text and the `AGENTS.md` library line, once 1 is answered | They must name the set; edited after the owner picks, in the same PR as the template. |
| 3 | Whether Grid stays in V1 | Waits on the scroll and Grid decision of #214. |
| 4 | Select's popup model: a dedicated listbox on the overlay host, or the native menu where one exists | Needs the non-modal focus and surface rules of #305 first. Select is the one V1 input not started for that reason. |
| 5 | Whether a Radio group is V1 or V1.1 | It is built; the question is only whether it gates the exit. |

## 5. What this slice changed

- Components: `Switch`, `Checkbox`, `Radio`, `RadioGroup` in `packages/tessera-ui/`, with
  `switch.css`, `checkbox.css` and `radio.css` (theme tokens only).
- Contract: controlled, because a callback prop carries no payload today: the caller owns the value
  and flips it in `onToggle` or `onSelect`. Keyboard activation is the runtime's: `ClickSynthesizer`
  clicks a focused `switch` (Space, Enter), `checkbox` and `radio` (Space) as it does a `button`
  (#1035), so the components carry no `onKeyDown`.
- Gallery: the `controls` page with goldens per theme and right-to-left
  (`tests/golden/gallery/controls_*.png`), a headless interaction test through the real
  `InputRouter`, and style and lowering checks.
- Radio roving (#1035): `RadioGroup` is a `focusGroup="both"`, so its radios are one Tab stop and the
  arrows move focus (skipping `aria-disabled` items, wrapping); `InputRouter` then clicks the radio an
  arrow landed on, so selection follows focus (Home/End only move). No group state in a component.
- Parity twin (#1035): `tools/parity/controls.html` against `gallery --page controls`
  (`parity_gate.py --page controls --width 640 --height 360`): structure difference 0.10 %, pixels
  2.83 % (lavapipe vs Chromium, Plex). Not done: Select (section 6).

## 6. Select: decision note (#1035)

Select stays unbuilt: its popup rides on #305, which is open. The recommendation for the owner (item 4
of section 4) is the **listbox on the overlay host**, not the native menu: the native menu does not
exist on Linux or in the Wasm preview, so it would fork the component per platform, while the overlay
host is the one popup path that runs everywhere and is already goldened. What the host still lacks,
all listed in #305, and what Select needs before code starts:

1. A way to open an overlay from TSX (a generated `open_overlay` with a component as `mount`) and to
   name the anchor, so the trigger button can place the list.
2. The non-modal `FocusPolicy` hook: focus moves to the selected option on open and returns to the
   trigger on close, with arrows roving the options (the `focusGroup` of #1035 already does the
   roving and skips `aria-disabled` options) and Escape dismissing.
3. `aria-live` in the a11y tree builder (done, `A11yNode::live`); the selection change sets it.

The trigger is a `button` with `aria-haspopup="listbox"`; the options are `option` nodes, so the
keyboard click synthesis added here (role-driven) extends to them with one more role.
