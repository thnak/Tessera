// SPDX-License-Identifier: MIT

// `TextFieldState`, the text-field-state behavior primitive (issue #236). It draws nothing of its
// own: one `group` View around a `TextInput` that owns the value, the focused state and the touched
// state (set on the first blur). They are exposed as the classes `focused` and `touched` for
// styling, and the input carries the accessible name and the required state.

import { useState } from "tessera";

export type TextFieldStateProps = {
  label: string;
  defaultValue?: string;
  placeholder?: string;
  required?: boolean;
};

export function TextFieldState({
  label,
  defaultValue = "",
  placeholder = "",
  required = false,
}: TextFieldStateProps) {
  const [value, setValue] = useState<string>(defaultValue);
  const [focused, setFocused] = useState<boolean>(false);
  const [touched, setTouched] = useState<boolean>(false);
  return (
    <View
      role="group"
      aria-label={label}
      className={focused ? "text-field focused" : touched ? "text-field touched" : "text-field"}
    >
      <TextInput
        aria-label={label}
        aria-required={required}
        value={value}
        placeholder={placeholder}
        onChange={setValue}
        onFocus={() => setFocused(true)}
        onBlur={() => {
          setFocused(false);
          setTouched(true);
        }}
      />
    </View>
  );
}
