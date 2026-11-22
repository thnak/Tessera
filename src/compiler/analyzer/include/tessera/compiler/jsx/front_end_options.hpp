// SPDX-License-Identifier: MIT

#pragma once

// Front-end switches (ADR-065, docs/specs/custom-hooks-v1.md).
namespace tessera::compiler::jsx {

struct FrontEndOptions {
    // Accept a project-declared `use...` function as a custom hook and expand it at each call site
    // (docs/specs/custom-hooks-v1.md). On by default since the expansion pass landed (#645): a
    // custom hook is a source-to-source rewrite, so a project that uses one produces the IR of the
    // hand-written component and a project that uses none is byte-identical to before.
    //
    // The switch remains so a caller can pin the old rejection: with it off a `use...` function is
    // `TESSERA_REACT_CUSTOM_HOOK` (jsx-subset-v1.md section 7), which is what the pre-#645 goldens
    // record.
    bool custom_hooks = true;
};

} // namespace tessera::compiler::jsx
