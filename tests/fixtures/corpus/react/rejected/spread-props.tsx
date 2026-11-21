// SPDX-License-Identifier: MIT

// Corpus fixture: spread props `{...props}`. ADR-066 item 8 keeps this rejected: keys must be known
// at compile time, so the port passes named props.

export function Wrapped({ ...props }: { label: string }) {
  return (
    <div className="wrapped">
      <Inner {...props} />
    </div>
  );
}
