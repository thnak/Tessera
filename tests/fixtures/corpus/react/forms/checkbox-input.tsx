// SPDX-License-Identifier: MIT

// Corpus fixture: `checked` and `type="checkbox"`. ADR-066 section 3 keeps these rejected as raw
// intrinsics: the library components Checkbox and RadioList provide them (ADR-064 tiers 3 to 5).

export function AcceptTerms({ accepted, onChange }: { accepted: boolean; onChange: () => void }) {
  return (
    <div className="accept-terms">
      <input type="checkbox" checked={accepted} onChange={onChange} />
      <span className="accept-label">I accept the terms</span>
    </div>
  );
}
