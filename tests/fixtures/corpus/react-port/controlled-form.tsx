// SPDX-License-Identifier: MIT

// Corpus (spike #660): controlled form with submit and preventDefault

import { FormEvent, useState } from "react";

export function LoginForm({ onLogin }: { onLogin: (user: string) => void }) {
  const [user, setUser] = useState("");
  const [pass, setPass] = useState("");
  function submit(e: FormEvent) {
    e.preventDefault();
    onLogin(user);
  }
  return (
    <form onSubmit={submit}>
      <input value={user} onChange={(e) => setUser(e.target.value)} />
      <input type="password" value={pass} onChange={(e) => setPass(e.target.value)} />
      <button type="submit">Sign in</button>
    </form>
  );
}
