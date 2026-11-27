// SPDX-License-Identifier: MIT

// Corpus (spike #660): map with index as key

export function Menu({ entries }: { entries: string[] }) {
  return (
    <ul className="menu">
      {entries.map((entry, i) => (
        <li key={i}>{entry}</li>
      ))}
    </ul>
  );
}
