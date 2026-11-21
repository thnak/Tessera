// SPDX-License-Identifier: MIT

// Corpus fixture: a template string in a render expression. Accepted today; it is the control the
// string-method fixture is read against.

export function Summary({ name, count }: { name: string; count: int }) {
  return (
    <p className="summary">
      {`${name} has ${count} items`}
    </p>
  );
}
