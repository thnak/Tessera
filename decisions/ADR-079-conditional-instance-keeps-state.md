# ADR-079: A conditional component instance keeps its state (keep-alive branch slots)

- Status: Proposed (2026-11-28). Owner-gated: it makes an already normative sentence of
  element-transitions-v1 section 3.2 say what the engine does. The behavior below is already
  implemented and tested; accepting this ADR changes no code.
- Date: 2026-11-28
- Related: [element-transitions-v1](../docs/specs/element-transitions-v1.md) section 3.2,
  [compiler-node-state.md](../docs/design/compiler-node-state.md) section 2,
  [compiler.md](../docs/design/compiler.md) (React-like, not React-identical),
  [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md). Issue: #283 (found while landing #231).

## Context

The spec says that without `presence`, a false condition destroys the node at once. The engine does
not do that for any conditional: codegen builds every branch subtree once, keeps it in a branch slot,
and attaches or detaches it by condition, so a hidden branch keeps its stable `node::N` id. For a
conditional **component instance** the instance object is a caller member whose host node moves in and
out of the slot, so its state, effects and refs survive every toggle. The runtime test
`codegen_instances_runtime` asserts that ("the instance state survived the toggle"). The spec sentence
was therefore already wrong for elements and contradicts the instance behavior.

## Decision (recommended)

1. **Keep-alive for elements and instances alike.** A conditional instance keeps its state while the
   condition is false. There is no element/instance difference.
2. A hidden instance is detached from the tree: it is not drawn, not hit-tested and not in the
   accessibility tree, but it stays mounted, so effects do not re-run and cleanups do not fire on a
   toggle. Authors who want a reset use `key` on the instance, which remounts it.
3. `presence` (AnimatePresence) is unchanged: it adds the `Exiting` state on top of a branch slot
   and does not depend on destroy-on-false.
4. This is a documented divergence from React (destroy and remount). Section 3 of compiler.md already
   states the model is React-like, not React-identical; the divergence is listed there.

## Consequences

- A half-filled form inside a conditional survives a hide and re-show. Ports of React code that rely
  on a reset by unmount must add a `key` (a class B port note under ADR-058).
- No node or instance is created or destroyed per toggle, which suits the no-hidden-allocation frame
  rule and keeps node ids stable for automation.
- Memory for a never-shown branch is paid once at build time; a large rarely shown subtree is a cost
  the author controls by splitting it into a lazily mounted route or list.

## Alternatives

- **Destroy on false (React-compatible).** Rejected as the default: it needs the branch-slot path for
  instances re-architected so the instance owner tears down and rebuilds on each toggle, runs
  `useEffect` cleanups per toggle, allocates per toggle, and `codegen_instances_runtime` flips to
  "state resets". If the owner chooses it, those are the changes, and the element sentence changes too.
- **Per-use opt-in** (`<Card keepAlive={false}>`). Rejected for V1: two lifetimes per construct, and
  `key` already expresses a reset.

## Follow-up

- Owner: accept or choose destroy-on-false. Until then the spec carries the keep-alive sentence with a
  pointer here.
- If accepted, an ADR-058 port note for the reset-by-unmount idiom.
