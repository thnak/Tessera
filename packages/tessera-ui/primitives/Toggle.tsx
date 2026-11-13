// SPDX-License-Identifier: MIT

// `Toggle`, the toggle behavior primitive (issue #236). It draws nothing: one focusable `switch`
// View that owns the checked state and exposes it as `aria-checked`. Pointer, Enter and Space
// arrive as `onClick` (the runtime synthesizes it for a focusable `switch`), and the handler flips
// the state. Disabling is the styled component's job (a plain `View` has no `disabled` prop).

import { Children, useState } from "tessera";

export type ToggleProps = {
  label: string;
  defaultChecked?: boolean;
  children: Children;
};

export function Toggle({ label, defaultChecked = false, children }: ToggleProps) {
  const [checked, setChecked] = useState<boolean>(defaultChecked);
  return (
    <View
      role="switch"
      tabIndex={0}
      aria-label={label}
      aria-checked={checked ? "true" : "false"}
      onClick={() => setChecked(!checked)}
    >
      {children}
    </View>
  );
}
