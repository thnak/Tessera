// SPDX-License-Identifier: MIT

// `Form`, the second first-party library component (docs/design/component-library.md; docs/specs/
// forms-and-validation-v1.md section 5.2). This file is the source of truth for the IR declaration
// that `make_library_component()` in src/compiler/lowering/src/library_components.cpp used to build
// by hand with magic ids and synthetic spans (issue #70). tests/unit/library_tsx_parity_tests.cpp
// compiles this file through the ordinary parser + analyzer + lowering path and pins its IR against
// that declaration.
//
// Form is presentational: one `form` container plus the static Children slot that carries the
// caller's Fields and submit controls. It owns no validation and no state; the generated
// FormController binding in the component that owns the instance does (docs/design/compiler.md
// section 11).

import { Children } from "tessera";

// The documented form prop records (forms section 5.2). `initial` is the field seed list as
// `name=value` pairs in declaration order; IR v1 has no record type, so the documented object
// literal `{{ email: "" }}` is not lowered and the `list<string>` form is the canonical encoding.
// Its default is the empty list, authored as the destructuring default below; the IR records a
// default on `initial` only (`?` marks it alone).
//
// `validate` and `onSubmit` are payload-free callback props: the IR records the type `callback`
// (codegen emits `std::function<void()>`). The generated binding invokes `validate` after the
// controller validates and `onSubmit` only after a successful submit.
export type FormProps = {
  initial?: string[];
  validate: () => void;
  onSubmit: () => void;
  children: Children;
};

// The tree is the hand-written one: the `form` container and its Slot.
export function Form({ initial = [], validate, onSubmit, children }: FormProps) {
  return (
    <View className="form">
      {children}
    </View>
  );
}
