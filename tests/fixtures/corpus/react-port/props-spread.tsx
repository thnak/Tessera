// SPDX-License-Identifier: MIT

// Corpus (spike #660): spreading props onto an element

export function Link(props: { href: string; label: string }) {
  const { label, ...rest } = props;
  return <a {...rest}>{label}</a>;
}
