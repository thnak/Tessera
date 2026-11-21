// SPDX-License-Identifier: MIT

// Corpus fixture: `dangerouslySetInnerHTML`. ADR-066 item 8 keeps this rejected: there is no V1
// meaning for raw markup.

export function RawHtml({ html }: { html: string }) {
  return (
    <div className="raw-html" dangerouslySetInnerHTML={{ __html: html }} />
  );
}
