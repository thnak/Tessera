# Component spec template

Copy this file to `docs/specs/components/<Name>.md`, fill every section and delete the guidance in
italics. A section with nothing to say states why in one line; it is not removed. The template follows
[ADR-064](../../../decisions/ADR-064-astryx-style-component-system.md) item 4. Rule ids are stable: a
rule that is removed keeps its id as "retired", so tests and issues can still cite it.

```text
---
component: <Name>
family: <Action | Container | Content | Feedback and Status | Form Controls | Layout | Navigation | Overlay | Table and List | Utility>
tier: <0-6, ADR-064 item 3>
status: <draft | current | retired>
implementation: <none | partial | complete, and the package path>
verified_by: [<ctest name or golden path>, ...]
---
```

## 1. Contract at a glance

*A table of at most eight rows: public props, behavior, accessibility, themeability, compatibility
(what a SemVer-minor change may not break), and the rules that govern each.*

| Aspect | Contract | Rules |
|---|---|---|

## 2. Intent and ownership

*Why the component exists in two or three sentences, then two lists.*

- **Owns:** what this component decides (state, layout of its own parts, focus, keyboard).
- **Does not own:** what a parent, a theme or another component decides, and which one.

## 3. Public concepts

*One row per prop, slot or event. Closed values are listed in full.*

| Concept | Closed values or type | Default | Meaning | Invalid value |
|---|---|---|---|---|

## 4. Behavior rules

*Numbered `FR1`, `FR2`, ... Each is one testable sentence with MUST or SHOULD, and a short reason.*

| Id | Rule | Reason |
|---|---|---|

*States table: every state the component has (rest, hover, pressed, focus-visible, disabled, pending,
invalid, selected, open, ...) and which attribute or pseudo-class carries it.*

| State | Carried by | Painted by |
|---|---|---|

*Precedence rules `ORD1`, ... when two inputs disagree (for example disabled and pending). Allowed
variation `AV1`, ...: what an implementation or theme may change without breaking the contract.
Performance rules `PR1`, ...: budgets and what must not allocate on the hot path.*

## 5. Accessibility rules

*Numbered `AR1`, ...: role, name source, keyboard, focus visibility, announcements, target size,
forced colors and high contrast. Cite the WCAG 2.2 criterion where one applies.*

| Id | Rule | Criterion |
|---|---|---|

## 6. Anatomy, parts and theming

*Every named part of the component appears here exactly once. A part is mapped to one of: `target`
(a theme can style it with `::part(name)`), `inherits` (it takes its style from the host), `delegatesTo`
(another component owns it: name it) or `none` (with a reason: `intentional`, `reachability-gap` or
`unsettled`).*

| Part | Mapping | Notes |
|---|---|---|

- **Attributes:** the public `data-*` and ARIA attributes this component sets (each is API).
- **Variables:** public `--<component>-*` variables with defaults; private ones start with `--_`.
- **Theme keys:** `base`, `variant:x`, `variant:x+size:y` and state names a theme may use.
- **Tokens read:** the role tokens it uses, so a theme author knows what to supply.

## 7. Relationships

*Composition (which components it uses), hooks it calls (name each), families it belongs to, and the
overlay or layer behavior it relies on.*

## 8. Verification map

*Every `FR`, `AR` and `ORD` id appears once with the test that proves it, the states that test covers
and what failure looks like. A rule with no test says "Evidence gap" and links an issue.*

| Rule | Test | States | Failure signal |
|---|---|---|---|

## 9. Current state

*What is implemented now, the gaps against sections 3 to 6, and the issue for each gap.*

## 10. Decisions and open questions

*A numbered decision log `DEC1`, ... (one line each, with the reason) and open questions for the
owner.*
