// SPDX-License-Identifier: MIT

// Corpus fixture: a custom hook declared in the same file. ADR-066 item 2 (ADR-065) proposes
// compile-time inlining at each call site; the fixture measures the declaration form.

export function useCounter(start: int) {
  return start + 1;
}

export function Counter({ start }: { start: int }) {
  const next = useCounter(start);
  return (
    <div className="counter">
      <span className="counter-next">{next}</span>
    </div>
  );
}
