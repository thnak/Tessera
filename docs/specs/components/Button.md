# Button

The first component spec in the [template](_template.md) format, written against the target contract
of [ADR-064](../../../decisions/ADR-064-astryx-style-component-system.md). Section 9 records what
`packages/tessera-ui/Button.tsx` does today and what is missing.

```text
component: Button
family: Action
tier: 3
status: draft
implementation: partial (packages/tessera-ui/Button.tsx, button.css)
verified_by: [library_button_lowering, library_button_style, library_button_runtime]
```

## 1. Contract at a glance

| Aspect | Contract | Rules |
|---|---|---|
| Props | `label`, `onPress`, `variant`, `size`, `disabled`, `loading`, `icon`, `endContent`, `iconOnly`, `tooltip`, `href` | section 3 |
| Behavior | One press action; pointer, touch and Enter/Space activate it the same way | FR1 to FR4 |
| Pending | `loading` shows a spinner, keeps the outer size and blocks activation | FR6, ORD1 |
| Accessibility | Role `button` (link when `href`), a name, one tab stop, a visible focus ring | AR1 to AR6 |
| Theming | One target (`button`); variant, size and state through attributes | section 6 |
| Compatibility | A closed value is never removed in a minor release; a new variant needs a theme key | AV1 |

## 2. Intent and ownership

A Button starts one action. It is not a toggle (ToggleButton), a navigation link (Link) or a group
(ButtonGroup).

- **Owns:** press state, activation, the pending state, its own parts, focus and keyboard.
- **Does not own:** the colours and radius (the theme), the spacing between buttons (the parent
  container), a tooltip's content and placement (Tooltip), the spinner's drawing (Spinner).

## 3. Public concepts

| Concept | Closed values or type | Default | Meaning | Invalid value |
|---|---|---|---|---|
| `label` | text | required | Visible text and, for an icon-only Button, the accessible name | Compile error when missing |
| `variant` | `primary`, `secondary`, `ghost`, `destructive` | `secondary` | Visual weight and intent | Falls back to `secondary` with a dev diagnostic |
| `size` | `sm`, `md`, `lg` | `md` | Height from the element size tokens | Falls back to `md` with a dev diagnostic |
| `disabled` | boolean | false | Not activatable, still focusable | n/a |
| `loading` | boolean | false | Pending state (FR6) | n/a |
| `icon`, `endContent` | an Icon or a node | none | Leading and trailing content | n/a |
| `iconOnly` | boolean | false | Square button, the label becomes the name only | n/a |
| `tooltip` | text | none | Delegates to Tooltip | n/a |
| `href` | string | none | Renders as a link (AR1) | n/a |
| `onPress` | `() => void` | required unless `href` | Called once per activation | Compile error |

## 4. Behavior rules

| Id | Rule | Reason |
|---|---|---|
| FR1 | A pointer press, a touch tap and Enter or Space call `onPress` once per activation. | One path for every input. |
| FR2 | Activation happens on release over the Button (pointer) or key up of Space; Enter activates on key down. | Matches a browser `button`; lets a press be cancelled by moving away. |
| FR3 | A press that leaves the Button before release does not activate and clears the pressed state. | Matches a browser. |
| FR4 | While `disabled` or `loading`, `onPress` is never called and the Button never shows as pressed. | Prevents double submit and false feedback. |
| FR5 | The Button is a single flex row: `icon`, `label`, `endContent`; the label does not wrap. | Predictable size. |
| FR6 | `loading` draws a Spinner over the content, the content stays in layout and becomes transparent, and the outer width and height do not change. | A pending state never shifts layout (ADR-064 item 9). |
| FR7 | A `href` Button navigates on activation and is not activatable while disabled. | A link must not do work when disabled. |
| FR8 | Inside ButtonGroup the Button takes its corners and divider from the group. | The group owns the joint. |
| AV1 | A theme may change colour, radius, border, shadow and the pressed treatment. It may not change which states exist or the minimum target size. | Keeps behavior portable across themes. |

| State | Carried by | Painted by |
|---|---|---|
| rest | none | `base` |
| hover | `:hover` (pointer devices only) | `base`, `variant:x` |
| pressed | `:active` (pointer), `data-pressed` (key) | `variant:x` |
| focus-visible | `:focus-visible` | the shared focus ring tokens |
| disabled | `aria-disabled="true"`, `data-disabled` | `base` |
| pending | `aria-busy="true"`, `data-loading` | `base` |

| Id | Rule |
|---|---|
| ORD1 | `loading` wins over `disabled` for painting (the spinner shows); both block activation. |
| ORD2 | `disabled` beats hover and pressed: no hover or press paint. |

## 5. Accessibility rules

| Id | Rule | Criterion |
|---|---|---|
| AR1 | Role `button`; with `href` and not disabled the role is `link`. | 4.1.2 |
| AR2 | The accessible name is `label`; an icon-only Button uses `label` as the name and shows no text. | 4.1.2 |
| AR3 | One tab stop. A disabled Button stays focusable and reports `aria-disabled`; it is never removed from the focus order. | 2.1.1 |
| AR4 | A pending Button reports `aria-busy` and announces the start and end of loading through `useAnnounce` (polite). | 4.1.3 |
| AR5 | The focus ring is at least 2 px, offset from the edge, 3:1 against both the Button and the surface, and visible in every variant. | 1.4.11, 2.4.7 |
| AR6 | The target is at least 24 by 24 px; the default `md` is 32 px high. High contrast adds a 1 px system border to solid variants. | 2.5.8, 1.4.11 |

## 6. Anatomy, parts and theming

| Part | Mapping | Notes |
|---|---|---|
| `button` | target | The host box; carries variant, size and state attributes. |
| `icon` | inherits | Takes the text colour and size. |
| `label` | inherits | Takes the font from the theme's `label` role. |
| `end-content` | inherits | Same as `icon`. |
| `spinner` | delegatesTo Spinner | Painted by Spinner; positioned over the content. |

- **Attributes:** `data-variant`, `data-size`, `data-loading`, `data-disabled`, `data-pressed`,
  `aria-disabled`, `aria-busy`.
- **Variables:** `--button-focus-offset` (default `var(--focus-outline-offset)`),
  `--button-icon-only-aspect` (default `1 / 1`). Private: `--_button-radius`.
- **Theme keys:** `base`, `variant:primary`, `variant:secondary`, `variant:ghost`,
  `variant:destructive`, `size:sm`, `size:lg`, `variant:x+size:y`, and the state names above.
- **Tokens read:** `--color-accent`, `--color-on-accent`, `--color-background-surface`,
  `--color-text-primary`, `--color-border`, `--color-error`, `--color-on-error`, `--size-element-*`,
  `--radius-element`, `--spacing-*`, the `label` font role, focus tokens, `--shadow-*`.

## 7. Relationships

- **Composes:** Icon, Spinner, Tooltip (and Badge as `endContent`).
- **Hooks:** `usePressFeedback`, `useAnnounce`, `useMergedRefs`, `useInteractiveRole`.
- **Families:** Action. Parent contexts: ButtonGroup, Toolbar, Dialog actions, Field (clear button).

## 8. Verification map

| Rule | Test | States | Failure signal |
|---|---|---|---|
| FR1, FR4 | `library_button_runtime` | rest, disabled | `onPress` count wrong |
| FR2, FR3 | Evidence gap (needs a release-cancel test) | pressed | Activation on leave |
| FR5 | `library_button_style` | rest | Wrapped label or size drift |
| FR6, ORD1, AR4 | Evidence gap: no `loading` yet | pending | Layout shifts |
| FR7, FR8 | Evidence gap: no `href`, no ButtonGroup yet | n/a | n/a |
| AR1 to AR3 | `library_button_lowering` (role, name, focusable) | rest, disabled | Wrong role or not focusable |
| AR5, AR6 | Evidence gap: no focus-ring contrast test | focus | Ring invisible |

## 9. Current state

`Button.tsx` takes `label`, `onPress`, `variant` (`primary`, `secondary`, `ghost`, `danger`), `size`
(`md`, `sm`), `disabled` and `children`. It is a `View` with `role="button"`, `tabIndex`, `aria-label`
and `aria-disabled`, activates on `onClick`, and styles pressed through `:active` and the `tbtn-pressed`
class. Gaps against this spec:

- `loading`, `icon`, `endContent`, `iconOnly`, `tooltip` and `href` do not exist (tiers 1 to 2 and the
  `useAnnounce` hook, #629, come first).
- The variant is named `danger`; this spec says `destructive`. Rename with a deprecation alias.
- There is no `lg` size, and sizes use `--control-height*` tokens; the target uses `--size-element-*`
  (the token migration in ADR-064 item 6).
- Activation is on `click`, not on key up or release (FR2, FR3).
- `aria-label` is always set, which hides a differing visible label from some tools; the name should
  follow AR2.
- Hover uses `opacity: 0.92`, not an overlay token.

## 10. Decisions and open questions

- **DEC1:** one theme target (`button`), as parts such as `icon` and `label` inherit; a theme restyles
  them through the host.
- **DEC2:** a disabled Button stays focusable (AR3), so a tooltip can explain why.
- **Open:** whether `lg` ships in the first release; whether `elevation` is a prop or a theme concern.
