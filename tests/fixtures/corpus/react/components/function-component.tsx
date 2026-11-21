// SPDX-License-Identifier: MIT

// Corpus fixture: the plain function component with destructured props and defaults. This is the
// form the subset already accepts; it is the control the other fixtures are read against.

export type GreetingProps = {
  name: string;
  tone?: string;
  count?: int;
};

export function Greeting({ name, tone = "neutral", count = 0 }: GreetingProps) {
  return (
    <div className={`greeting greeting-${tone}`}>
      <span className="greeting-name">{name}</span>
      <span className="greeting-count">{count}</span>
    </div>
  );
}
