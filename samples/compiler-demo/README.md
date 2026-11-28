# compiler-demo

A small screen used by the codegen CLI sample and the Big Step C acceptance: `useState`,
`useEffect` (deps and cleanup), `useRef`, `useAnimatedValue`, a declared handler, inline setter
handlers and a conditional child. `Demo.cpp` / `Demo.hpp` are the developer-owned class beside the
generated base (ADR-040).

It ships `demo.css` (issue #607), imported from `Demo.tsx`, so the Studio's first sample shows author
styling instead of the fallback sheet; any test that copies `Demo.tsx` alone must copy the sheet too.

It has no `tessera.json` on purpose: a manifest's `app.namespace` is the automation-id salt, and the
codegen CLI and integration tests build this screen unsalted. The Studio opens the folder anyway
(a lone `Demo.tsx` needs no manifest). See
[click-demo](../click-demo/README.md) for the native app that drives it.
