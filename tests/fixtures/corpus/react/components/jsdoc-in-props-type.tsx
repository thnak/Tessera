// SPDX-License-Identifier: MIT

// Corpus fixture: a JSDoc comment inside a props type. Not in `react-core-coverage.md`: the type
// grammar has no comment form inside an object type, so a documented prop is a parse error. The port
// moves the comment above the type alias.

export function WelcomeView({
  choices,
}: {
  /** The sample entries of the folder just picked, when it holds several; empty otherwise. */
  choices: string[];
}) {
  return (
    <div className="welcome">
      <span className="welcome-count">{choices.length}</span>
    </div>
  );
}
