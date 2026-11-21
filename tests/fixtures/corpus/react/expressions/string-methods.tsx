// SPDX-License-Identifier: MIT

// Corpus fixture: string methods in a render expression. ADR-066 item 3 proposes the total
// allowlist (`trim`, `toUpperCase`, `toLowerCase`, `startsWith`, `slice`, `toFixed`).

export function Initials({ name, price }: { name: string; price: float }) {
  return (
    <div className="initials">
      <span className="initials-name">{name.trim().toUpperCase()}</span>
      <span className="initials-price">{price.toFixed(2)}</span>
    </div>
  );
}
