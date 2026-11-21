// SPDX-License-Identifier: MIT

// Corpus fixture: a destructured local `const { state } = io;`. Not in `react-core-coverage.md`:
// the parser accepts only a name or `[` after `const`, so an object pattern is a parse error, and
// the port has to write `const state = io.state;` instead.

export function ProjectBar({ io }: { io: string }) {
  const { state } = io;
  return (
    <div className="project-bar">
      <span className="project-name">{state}</span>
    </div>
  );
}
