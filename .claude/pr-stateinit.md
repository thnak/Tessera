# compiler: useState initializer that reads a prop starts at the caller's prop; Slider disabled

## What changed
- Codegen (#1055): a state whose initial reads a prop (`prop::` ref anywhere in the expression) gets a `state_x_seeded_` flag and is seeded once in the generated `build()`, after the owner's `apply_props()` and before any child is built. States that read no prop keep the member initializer. Goldens for Dynamic, VirtualDemo and record/FormPage regenerated (they read a prop in a state initial).
- Test: `codegen_state_init` compiles Counter (int and string states reading props) and a Host passing `start={7} label="seven"` through the real front end and generated C++; it fails without the fix (verified), and covers the default instance and a standalone component.
- Spec note in docs/specs/component-model-v1.md section 2.
- Slider primitive (#1051, partial): `disabled` prop reported as `aria-disabled`; public-api baseline +1.

## Verification
- Full vs2026 Release build; ctest: only the known TEMP-dependent tests (0xc0000409 group, icons_import) fail locally, plus gradient_gpu (no GPU path touched).
- `python -B scripts/gates.py --quiet` 15/15; scripts/format.ps1 clean. No Linux/GCC run.

## What is left
- #1067: replace the Range/Details workarounds (those files are on the #1041 branch, not this base).
- #1069: controlled range value/onChange (needs an int-payload callback prop), styled Slider composing the primitive, goldens, html-aliases rows.

Refs #1055, Refs #1051
