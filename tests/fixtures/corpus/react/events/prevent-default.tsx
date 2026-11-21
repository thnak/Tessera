// SPDX-License-Identifier: MIT

// Corpus fixture: `e.preventDefault()` and `e.stopPropagation()` in a native handler. ADR-066
// section 3 keeps these native-handler-only (Q3); the fixture measures the escape hatch.

export function FormShell({ onSubmit }: { onSubmit: () => void }) {
  return (
    <div className="form-shell">
      <button
        onClick={(e: PointerEvent) => {
          e.preventDefault();
          e.stopPropagation();
          onSubmit();
        }}
      >
        Send
      </button>
    </div>
  );
}
