# Service injection: `useService<T>()`

Part of the [services design set](README.md). Sections 11.5-11.6 are the AF5 codegen slice and
what the TSX half still needs; 11.5 depends on the contract in
[`common-contract.md`](common-contract.md) section 1.1. Both headings keep their original numbering
and level, so `section 11.5` and `section 11.6` still name them.

### 11.5 `useService<T>()`: what codegen emits (AF5, issue #177)

AF5 lands the IR-to-codegen half of ADR-050 section 3.1. The TSX half is a separate slice; section
11.6 states exactly what it needs and what this slice deliberately does not invent.

**The TSX contract becomes IR data, not a codegen convention.** `ir::Component::services` is an
ordered list of `{id, name, type, header, source}` entries. `type` and `header` are the C++ spelling
of the interface (`demo::Navigator`, `demo/services.hpp`), which the project's `ui/services.json`
manifest supplies (ADR-050 section 3.1 item 1); the IR carries them as data because codegen reads
the IR only (AGENTS.md non-negotiable 4). The field is additive and omitted from the IR JSON when
empty, so a module without `useService` is byte-identical to the IR before it existed. This is
deliberately *not* `ir::Environment`: an environment value is read through a provider on every use,
a service is a typed pointer cached at mount.

**The generated base**, for a component with N requirements:

```cpp
std::tuple<demo::Navigator*, demo::MailStore*> services_ = {};   // private, once
void attach_services(const tessera::appfw::ServiceContext& context);  // N lookups, once
void mount(const tessera::appfw::ServiceContext& context);            // attach, then mount()
template <class T> [[nodiscard]] T& service() const;                  // cached pointer
[[nodiscard]] demo::Navigator& nav() const;                           // one per requirement
```

Two decisions are worth naming. **The context is not retained**: after `attach_services()` the only
state is the typed pointers, so "no resolve in the frame path" (ADR-050 section 3 item 5) is
structural rather than a review rule -- there is no registry left to look up from. And **the
diagnostics live in `service<T>()`, not in `build()`**: they are properties of one call, not of the
component's lifetime, so a component that declares a requirement but never reads it still compiles.

| Code | When | Message shape |
|---|---|---|
| `TESSERA_CG_SERVICE_UNDECLARED` | `service<T>()` for a valid interface the component never declared | names the component and every declared requirement with its TSX `file:line:column` |
| `TESSERA_CG_SERVICE_NOT_INTERFACE` | `service<T>()` for a type with no `kServiceId` (ADR-050 section 3 item 2: never a concrete type or a string key) | names the component and states the `kServiceId` requirement |

Both are `static_assert`s carrying the code, the same convention as `TESSERA_CG_MISSING_BINDING`
(`emit_base.cpp`). They are instantiated by a nested `ServiceRequirement<T>` struct, so the
compiler's own instantiation trace names the offending type next to the message: a developer sees
`ServiceRequirement<demo::Settings>` and `service<demo::Settings>` at their call site, not a bare
template dump. A type that is not an interface fails only the interface assert, so one mistake gives
one diagnostic instead of a second, misleading "not declared" for the same name.

**Emission is conditional on the component having a requirement.** A component that does not call
`useService` generates not one byte of service code -- no include, no slot, no accessor -- which is
why every pre-existing `.g.hpp` golden stays byte-identical (`git status tests/golden` shows only
the new `tests/golden/codegen/services/` directory).

**The resolve cost is checked twice, and neither check is a wall-clock gate.** The deterministic gate
is `codegen_services_runtime`: with AF1's `service_lookup_audit()` counters, 1 000 accessor reads
inside a `FrameScope` add **zero** lookups and zero misses, and the same holds for a read from a
generated event handler -- paired with value checks (the provided instance's state is observed
through the accessor), so an accessor that did nothing could not pass. The documented measurement is
[`service-resolve-benchmark.md`](../service-resolve-benchmark.md) section 4.5: an accessor read is
0.37 ns, the same machine code as the cached-pointer case, while resolving from a retained context on
every use costs 4.61 ns, about 12x even with the registry entirely in cache.

### 11.6 The TSX half of `useService<T>()`: what is still missing

Not in this slice, and not guessable from the ADR:

1. **Call-expression lowering.** A service use is `nav.push(route)`, and `lower_reference` gives
   member chains an IR form only for a prop object, a record-typed state and a loop item
   (`reference_lowering.cpp`); a call on that chain has no IR expression today, and
   `TESSERA_JSX_SUBSET_CALL` rejects it at the front end. `useService` is therefore only useful once
   a member call on a service value is an IR expression.
2. **A hook form.** `useService<T>()` needs an entry in `rules` (`check_hook_statement`), a
   `SymbolKind`, and a `lower_hooks` branch that appends to `ir::Component::services`. Its `T` is a
   type argument the front end currently only parses for `useState`/`useRef`.
3. **The manifest and its typings.** `ui/services.json` (id, C++ class, header, the methods TSX may
   call with their IR types) plus the generated `services.d.ts` is a spec-level artifact with no
   format in the repository yet; ADR-050 section 3.1 item 1 names what it must contain but not its
   schema. Writing one here would be an ADR/spec decision, so the IR entry took `type` and `header`
   as data and left the reader to that slice.
4. **Startup validation of missing providers**, which needs the manifest's union across components to
   know what to require (ADR-050 section 3.1 item 6). AF1's runtime form of the same failure already
   terminates with `TESSERA_APPFW_SERVICE_MISSING`.
