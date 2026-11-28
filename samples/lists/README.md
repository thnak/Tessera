# lists

Issue #607: a keyed `.map` over a list-typed state cell, a counter button and a conditional child. A loop item
is not yet usable inside an event handler and a top-level array constant is not lowered (both are
`TESSERA_IR_UNRESOLVED` today), so rows are plain. One component per file (the preview cannot take a second one), so a row is an element.
Browser twin: `tools/parity/lists.html`.
