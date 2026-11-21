// SPDX-License-Identifier: MIT

#pragma once

// Experimental front-end switches (spike #659, ADR-065). Every switch defaults to the behaviour the
// committed goldens record, so a caller that does not opt in sees the front end it saw before.
namespace tessera::compiler::jsx {

struct FrontEndOptions {
    // Accept a project-declared `use...` function as a custom hook and expand it at each call site
    // (docs/specs/custom-hooks-v1.md). Off by default: with it off such a function is the
    // `TESSERA_REACT_CUSTOM_HOOK` rejection of jsx-subset-v1.md section 7, unchanged.
    bool custom_hooks = false;
};

} // namespace tessera::compiler::jsx
