// SPDX-License-Identifier: MIT

// Corpus fixture: `??` over an optional value. ADR-066 item 1 proposes both `??` and `?.` as IR
// expressions with a type rule over `optional<T>`.

export function OptionalLabel({ label, fallback }: { label?: string; fallback: string }) {
  return (
    <div className="optional-label">
      <span className="optional-value">{label ?? fallback}</span>
    </div>
  );
}
