// SPDX-License-Identifier: MIT

// Corpus (spike #660): switch statement choosing JSX

type Status = "idle" | "busy" | "error";

export function StatusIcon({ status }: { status: Status }) {
  switch (status) {
    case "busy":
      return <span>...</span>;
    case "error":
      return <span>!</span>;
    default:
      return <span>ok</span>;
  }
}
