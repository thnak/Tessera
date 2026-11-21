// SPDX-License-Identifier: MIT

// Corpus fixture: an arrow-function component bound to a `const` and exported. The most common
// spelling in ported code, and the one ADR-066 item 1 proposes to accept.

export const Avatar = ({ initials, size = 32 }: { initials: string; size?: int }) => {
  return (
    <div className="avatar" style={{ width: size, height: size }}>
      <span className="avatar-initials">{initials}</span>
    </div>
  );
};
