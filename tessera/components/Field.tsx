// SPDX-License-Identifier: MIT

// `Field`, the first-party library component (docs/design/component-library.md; docs/specs/
// forms-and-validation-v1.md section 5.1). This file is the source of truth for the IR declaration
// that `make_library_component()` in src/compiler/lowering/src/library_components.cpp used to build
// by hand with magic ids and synthetic spans (issue #70). tests/unit/library_tsx_parity_tests.cpp
// compiles this file through the ordinary parser + analyzer + lowering path and pins its IR against
// that declaration.
//
// Field renders the programmatic label, the current value and the visible error above the caller's
// widget, which arrives in the static Children slot. It owns no validation and no state:
// `required`, `validators` and the error records are data the surrounding Form owns, never logic
// evaluated here.

import { Children, TextChangeEvent } from "tessera";

// The documented field prop records (forms section 5.1). The IR records a default value on every
// record except `onValidate` and `children`; a type alias has no default syntax in this subset, so
// `?` marks exactly the records that carry one. `validators`/`errors` default to the empty list.
//
// `onValidate` is a callback prop. The IR records its payload record `TextChangeEvent` as the prop
// type and binds it through the node's events, never as a value (codegen emits
// `std::function<void(const TextChangeEvent&)>`).
export type FieldProps = {
  name?: string;
  label?: string;
  value?: string;
  defaultValue?: string;
  required?: boolean;
  validators?: string[];
  onValidate: TextChangeEvent;
  error?: string;
  errors?: string[];
  children: Children;
};

// The tree is the hand-built one, in the same pre-order: the `field` container, then the label, the
// value and the error text, then the caller's widget at the Slot. Only the props the tree reads are
// destructured; the rest stay declared by `FieldProps`.
export function Field({ label, value, error, children }: FieldProps) {
  return (
    <View className="field">
      <Text>{label}</Text>
      <Text>{value}</Text>
      <Text>{error}</Text>
      {children}
    </View>
  );
}
