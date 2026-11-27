// SPDX-License-Identifier: MIT

// Corpus (spike #660): sorted copy then map

interface Row {
  id: string;
  score: number;
}

export function Leaderboard({ rows }: { rows: Row[] }) {
  const ranked = [...rows].sort((a, b) => b.score - a.score);
  return (
    <ol>
      {ranked.map((r) => (
        <li key={r.id}>
          {r.id}: {r.score}
        </li>
      ))}
    </ol>
  );
}
