// SPDX-License-Identifier: MIT

// Corpus (spike #660): short-circuit rendering with a string prop

export function Badge({ count }: { count: number }) {
  return (
    <span className="badge">
      Inbox
      {count > 0 && <em className="dot">{count}</em>}
    </span>
  );
}
