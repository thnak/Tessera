// SPDX-License-Identifier: MIT

// Corpus (spike #660): early return before the main JSX

export function Greeting({ name }: { name?: string }) {
  if (!name) {
    return <p>Please sign in</p>;
  }
  return <p>Welcome back, {name}</p>;
}
