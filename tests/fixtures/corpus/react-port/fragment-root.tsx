// SPDX-License-Identifier: MIT

// Corpus (spike #660): returned fragment with two siblings

export function Header({ title, subtitle }: { title: string; subtitle: string }) {
  return (
    <>
      <h1>{title}</h1>
      <h2>{subtitle}</h2>
    </>
  );
}
