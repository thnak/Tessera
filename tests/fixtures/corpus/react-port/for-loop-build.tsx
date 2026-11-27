// SPDX-License-Identifier: MIT

// Corpus (spike #660): a for loop that builds an array of elements

export function Stars({ n }: { n: number }) {
  const out = [];
  for (let i = 0; i < n; i++) {
    out.push(<span key={i}>*</span>);
  }
  return <div>{out}</div>;
}
