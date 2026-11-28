// SPDX-License-Identifier: MIT

// `Checkbox`, the checkbox behavior primitive (issue #544, ADR-059 group E). It draws nothing: one
// focusable `checkbox` View that owns the checked state and exposes it as `aria-checked`. Pointer,
// Enter and Space arrive as `onClick` (the runtime synthesizes it for a focusable `checkbox`), the
// handler flips the state and then tells the caller through the payload-free `onToggle` callback
// prop, which is how a form learns of the change. `label` is the accessible name; a `label
// htmlFor` that names this control's `id` forwards its click here (issue #542). Disabling is the
// styled component's job (a plain `View` has no `disabled` prop).

import { Children, useState } from "tessera";

export type CheckboxProps = {
  label: string;
  onToggle: () => void;
  defaultChecked?: boolean;
  children: Children;
};

export function Checkbox({ label, onToggle, defaultChecked = false, children }: CheckboxProps) {
  const [checked, setChecked] = useState<boolean>(defaultChecked);
  return (
    <View
      role="checkbox"
      tabIndex={0}
      aria-label={label}
      aria-checked={checked ? "true" : "false"}
      className={checked ? "checkbox checked" : "checkbox"}
      onClick={() => {
        setChecked(!checked);
        onToggle();
      }}
    >
      {children}
    </View>
  );
}
