// SPDX-License-Identifier: MIT
//
// Shared fixtures of the compiler-error overlay tests (issue #78 item 4): the document literals,
// the failure counter and the `check` helper, so the document tests and the host tests agree on the
// same bytes without either file owning a second copy.
//
// The documents are the spec's own examples (docs/specs/compiler-diagnostics-v1.md section 2 and
// section 3).

#pragma once

#include <iostream>
#include <string_view>

namespace tessera::test::compiler_error_overlay {

inline int failures = 0;

inline void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// The spec's section 2 example, byte for byte: one error with a full span and a suggestion.
inline constexpr std::string_view kErrorDocument = R"({
  "diagnostics": [
    {
      "code": "TESSERA_IR_UNSUPPORTED",
      "message": "`<Nope>` is neither an intrinsic element nor a component declared or imported in this file",
      "severity": "error",
      "span": {
        "file": "ui/Counter.tsx",
        "offset": 44,
        "length": 6,
        "line": 3,
        "column": 11,
        "endLine": 3,
        "endColumn": 17
      },
      "suggestion": "declare or import the component, or fix the tag"
    }
  ],
  "diagnosticsVersion": 1,
  "file": "ui/Counter.tsx"
})";

// Two errors, the second with no span: the overlay must render both and must not invent a location
// for the second (spec section 3).
inline constexpr std::string_view kTwoErrorDocument = R"({
  "diagnostics": [
    {"code": "TESSERA_JSX_UNTERMINATED", "message": "unterminated element", "severity": "error",
     "span": {"file": "ui/App.tsx", "offset": 0, "length": 4, "line": 12, "column": 5,
              "endLine": 12, "endColumn": 9}},
    {"code": "TESSERA_CFG_INVALID", "message": "tessera.json has no app.namespace",
     "severity": "error"}
  ],
  "diagnosticsVersion": 1,
  "file": "ui/App.tsx"
})";

inline constexpr std::string_view kWarningDocument = R"({
  "diagnostics": [
    {"code": "TESSERA_CG_STYLESHEET_NOT_EMBEDDED", "message": "stylesheet is not embedded",
     "severity": "warning"}
  ],
  "diagnosticsVersion": 1,
  "file": "ui/App.tsx"
})";

inline constexpr std::string_view kCleanDocument = R"({"diagnostics": [], "diagnosticsVersion": 1,
  "file": "ui/App.tsx"})";

} // namespace tessera::test::compiler_error_overlay
