# ADR-013: Project license is MIT

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-012](ADR-012-static-linking.md) (static linking)
- Supersedes: the "decision pending" note in [`docs/specs/dependencies.md`](../docs/specs/dependencies.md)

## Context

Tessera needs a project license before any public release. The dependency set is entirely
permissive (MIT, BSD, Apache-2.0, BSL-1.0, zlib, ISC, Unicode, FreeType, OFL, public domain),
and static linking is a hard requirement ([ADR-012](ADR-012-static-linking.md)). The license must allow:

- static linking of the framework and its dependencies into a single executable
- commercial and closed-source applications built with the framework
- redistribution of binaries with the required notices

The author's other projects use MIT, and MIT is the lowest-friction choice for adoption.

## Decision

1. **Tessera is licensed under the MIT License.** See [`LICENSE`](../LICENSE).
2. **All shipped dependencies must remain permissive** (MIT, BSD, Apache-2.0, BSL-1.0, zlib,
   ISC, Unicode, FreeType FTL, OFL, public domain). No GPL/AGPL in the core.
3. **Static linking obligations are honored** per [ADR-012](ADR-012-static-linking.md):
   - ship `THIRD_PARTY_NOTICES.txt` with binaries
   - expose the MIT text and third-party notices in an About/licenses UI
   - preserve Apache-2.0 `NOTICE`, MIT/BSD copyright, OFL font licenses, FreeType FTL
     notices, and Unicode/ICU notices
4. **SPDX (Software Package Data Exchange license identifiers) headers** identify the project license in source files:
   `SPDX-License-Identifier: MIT`.
5. **No copyleft in statically linked code.** LGPL only as an optional, dynamically loaded
   system integration only if a concrete requirement appears (none exists today), never as part of the static core.

## Consequences

### Positive

- Simplest permissive license; easy for individuals, startups, and enterprises to adopt.
- Compatible with every [V1](../docs/reference/glossary.md) dependency and with static linking.
- Consistent with the author's other repositories.
- Closed-source and commercial applications can link Tessera statically.

### Negative / risks

- MIT has **no express patent grant** (unlike Apache-2.0). If patent protection becomes a
  requirement, a superseding ADR must relicense the project.
- MIT still requires preserving copyright and permission notices in binary distributions;
  this must be automated, not forgotten.
- Apache-2.0 dependencies add their own `NOTICE` obligations.
- A relicensing (for example to Apache-2.0) requires the consent of all contributors
  unless a CLA (contributor license agreement) or DCO (Developer Certificate of Origin) policy is in place.

## Alternatives considered

- **Apache-2.0.** Better patent protection and corporate acceptance, but more process
  (NOTICE, state changes, patent termination). Rejected for simplicity; a superseding ADR can adopt it.
- **Dual MIT OR Apache-2.0.** Maximum flexibility, but more complexity than needed for V1.
- **GPL/AGPL.** Rejected: conflicts with broad adoption and closed-source embedding.
- **No license (all rights reserved).** Rejected: blocks use and contribution.

## Follow-up

- Keep `THIRD_PARTY_NOTICES.md` in sync with [`docs/specs/dependencies.md`](../docs/specs/dependencies.md).
- Add SPDX headers to new source files.
- Add a CI (continuous integration) license check and a generated notices step in [Big Step B](../AGENTS.md#big-steps-group-work-dont-micro-step) (core renderer).
- Decide on a contributor policy (DCO or CLA) before accepting outside contributions.
