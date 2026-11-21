// SPDX-License-Identifier: MIT

// Corpus fixture: `export default function`. A page-level component in most React apps, and the
// second spelling ADR-066 item 1 proposes to accept.

export default function SettingsPage({ title }: { title: string }) {
  return (
    <main className="page">
      <h1 className="page-title">{title}</h1>
    </main>
  );
}
