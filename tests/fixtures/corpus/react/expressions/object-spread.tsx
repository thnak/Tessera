// SPDX-License-Identifier: MIT

// Corpus fixture: object spread over a record state. ADR-066 item 3 proposes `{...state, x}` for
// record values with known fields (ADR-051); `useReducer` needs it.

import { useState } from "react";

type FormState = { name: string; email: string };

export function ProfileForm() {
  const [state, setState] = useState<FormState>({ name: "", email: "" });
  const rename = () => setState({ ...state, name: "Ada" });
  return (
    <div className="profile-form" onClick={rename}>
      <span className="profile-name">{state.name}</span>
    </div>
  );
}
