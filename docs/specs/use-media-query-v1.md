# `useMediaQuery` v1

Status: draft, normative once accepted. Specifies the `useMediaQuery` behavior hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #631). The
behavior-hook machinery it stands on is [`behavior-hooks-v1.md`](behavior-hooks-v1.md); the
`environment` lowering kind it uses is [`jsx-subset-v1.md`](jsx-subset-v1.md) section 7.1.

Terms: the **query** is the media query string the hook is called with; the **media context** is the
window description the cascade resolves `@media` against (`style::MediaContext`); the **cascade** is
the stylesheet engine's own `@media` evaluation.

## 1. Call form

```tsx
const wide = useMediaQuery("(min-width: 600px)");
const dark = useMediaQuery("(prefers-color-scheme: dark)");
return <View data-wide={wide}>{...}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The query is required and is a string expression; the hook returns a boolean.

## 2. Arguments

| Argument | Type | Meaning |
|---|---|---|
| `query` | `string` | a media query list, in CSS syntax |

The query is evaluated once per component update, not per frame: a query built from state re-parses
when the state changes, and a constant query parses once.

## 3. Outputs

| Output | Type | Meaning |
|---|---|---|
| (the binding) | `boolean` | whether the query matches the current media context |

## 4. Rules

Each rule is normative and has a test in `tests/unit/media_query_match_tests.cpp` (the engine half)
or `tests/unit/codegen_environment_tests.cpp` (the generated surface).

1. **One evaluator.** The hook parses and evaluates with `style::parse_media_query_list` and
   `style::evaluate` — the same two functions the cascade calls. There is no second parser and no
   second evaluator, so the hook and a `@media` rule cannot disagree about the same query.
2. **The same answer as the cascade.** For any query and any media context, the hook's boolean equals
   the cascade's answer for that query. This is the rule the issue states as "consistent with
   `useBreakpoint` and the stylesheet's media evaluation".
3. **The same answer as `useBreakpoint`.** A query written at a size-class threshold matches exactly
   when the size class is on the corresponding side of it, because both read
   `MediaContext::viewport_width` and the thresholds are the same named constants.
4. **A query list is a disjunction.** `"(max-width: 599px), (min-width: 1200px)"` matches when either
   query matches, as in CSS.
5. **A malformed query is `false`.** A query that does not parse never matches and never raises: a
   media query is a string a component may build at runtime, and a bad one must not take the frame
   down.
6. **An unevaluated query is `false`.** Before the host hands the component a media context, the
   field holds `false`, so a component renders its non-matching layout for the first frame rather
   than claiming a match it has not checked.
7. **The value is equality-gated.** Pushing the same boolean again is a no-op: a resize that does not
   cross the query costs one comparison and no re-render (ADR-041 decision 6).
8. **The query is parsed once per update.** The generated base holds one parsed matcher per hook, so
   a per-frame evaluation is one walk over the parsed list rather than a re-parse.
9. **The host supplies the context.** The component does not read the window: the host calls
   `set_environment_<name>_context(const style::MediaContext&)` with the same context the cascade
   resolves against, so a headless test drives the hook with a plain struct.
10. **No hidden global state.** The hook holds one parsed matcher and one boolean. It registers no
    listener and reads no clock.

## 5. Accessibility

- A media query is a **layout** decision, not a semantic one. A component that hides content with
  `useMediaQuery` must keep that content reachable another way (a disclosure, a menu), because
  `display: none` and a conditional render both remove it from the accessibility tree.
- `(prefers-reduced-motion: reduce)` is the one query with an accessibility obligation: a component
  that animates must honour it, and the engine already clamps animation when the context says so
  (ADR-034). The hook lets a component branch on the same answer.
- `(prefers-color-scheme: dark)` is a preference, not a guarantee: a component must not rely on it
  for contrast, because the user may have set it without the theme following.

## 6. States

| State | Field | Meaning |
|---|---|---|
| before the first context | `false` | not evaluated |
| context pushed, query matches | `true` | the query matches |
| context pushed, query does not match | `false` | the query does not match |
| malformed query | `false` | never matches |

## 7. Verification map

| Rule | Test |
|---|---|
| 1 | `test_agrees_with_the_cascade` (both call the same evaluator) |
| 2 | `test_agrees_with_the_cascade` |
| 3 | `test_agrees_with_the_size_class` |
| 4 | `test_query_list_is_a_disjunction` |
| 5 | `test_malformed_query_is_false` |
| 6 | `test_media_query_surface` (`bool env_wide_ = false;`) |
| 7 | `test_media_query_surface` (the setter is equality-gated) |
| 8 | `test_media_query_surface` (`matcher_.parse(...)` once, `matcher_.matches(context)` per push) |
| 9 | `test_media_query_surface` (`set_environment_wide_context`) |
| 10 | the whole file: every test builds its own context and asserts no shared state |

## 8. What this hook does not do

- It does not add a media feature the engine lacks. `prefers-contrast` is not a feature yet
  (ADR-064 item 6 records the gap), so a query using it does not parse and rule 5 applies.
- It does not subscribe to changes: the host pushes a new context, exactly as it pushes a new size
  class for `useBreakpoint`.
- It does not replace `useBreakpoint` for the coarse buckets: a component that wants
  compact/medium/expanded reads the class, not three queries.
