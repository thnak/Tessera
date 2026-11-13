// SPDX-License-Identifier: MIT

// `Press`, the press behavior primitive (issue #236, ADR-047 item 2). It draws nothing: one
// focusable `button` View that owns the accessibility contract (role, name) and the pressed state,
// which it exposes as the class `pressed` for styling. Pressed starts on pointer down, or on Enter
// or Space while focused, and ends on pointer up/leave or key up. Activation itself is the
// `onClick` the runtime already delivers for pointer, Enter and Space on a focusable `button`
// (src/ui/scene/click_synthesis.cpp).
//
// Known gap: a component cannot forward a callback prop to an inner event (`onClick={onPress}` is
// `TESSERA_IR_UNSUPPORTED` in jsx-subset-v1), so the caller's `onClick` cannot reach the node
// through this wrapper yet. A plain `View` also has no `disabled` prop; a disabled control is the
// styled component's job (it renders `Button`).

import { Children, KeyEvent, useState } from "tessera";

export type PressProps = {
  label: string;
  children: Children;
};

export function Press({ label, children }: PressProps) {
  const [pressed, setPressed] = useState<boolean>(false);
  return (
    <View
      role="button"
      tabIndex={0}
      aria-label={label}
      className={pressed ? "press pressed" : "press"}
      onPointerDown={() => setPressed(true)}
      onPointerUp={() => setPressed(false)}
      onPointerLeave={() => setPressed(false)}
      onKeyDown={(e: KeyEvent) => (e.key === "Enter" || e.key === " ") && setPressed(true)}
      onKeyUp={() => setPressed(false)}
    >
      {children}
    </View>
  );
}
