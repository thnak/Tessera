// SPDX-License-Identifier: MIT

// Corpus fixture: `readonly string[]` in a prop type. Not in `react-core-coverage.md`: the type
// grammar has no `readonly` modifier, so the parse fails before any React rule is reached. The port
// writes `string[]`.

export function ChoiceList({ choices }: { choices: readonly string[] }) {
  return (
    <div className="choice-list">
      {choices.map((choice) => (
        <span key={choice} className="choice">{choice}</span>
      ))}
    </div>
  );
}
