# Updates and distribution v1

Status: proposed (Big Step F, production breadth; see [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision:
[ADR-031](../../decisions/ADR-031-production-tooling-and-distribution.md) (production tooling and
distribution). Companion to
[`build-and-packaging.md`](../design/build-and-packaging.md), [`packaging.md`](../guides/packaging.md),
and [`security.md`](../design/security.md).

Terms: **V1** is the first release scope ([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8);
**v1.1** in this file means the release after V1. **SemVer** is Semantic Versioning (see
[`versioning-v1.md`](versioning-v1.md)). **SBOM** is a software bill of materials. **MDM** is
mobile/enterprise device management. **Health check** is defined in section 6.1. See also the
[glossary](../reference/glossary.md). Gap ids (`G-23`, `Q-22`) refer to rows in
[`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md).

Desktop apps need a safe update path; mobile apps use store updates but still need versioning,
signing, and rollback discipline.

## 1. Versioning and channels

- SemVer for the app; build metadata includes commit/toolchain/config hash.
- Channels: `stable`, `beta`, `nightly` (`nightly` is optional per app), per-platform.
- Update manifest is signed; channel/version/platform/arch are explicit.
- Downgrades require an explicit policy/signature; no silent rollback across incompatible data.
- Data/schema migrations run once and are forward-compatible (the cases where this cannot hold are not specified; see section 7, data migration guarantees).

## 2. Update manifest

```json
{
  "manifestVersion": 1,
  "channel": "stable",
  "version": "1.4.0",
  "platform": "windows-x64",
  "url": "https://updates.example.com/app-1.4.0.zip",
  "sha256": "...",
  "signature": "...",
  "minOs": "10.0.19041",
  "notesUrl": "https://example.com/releases/1.4.0"
}
```

- HTTPS only; certificate/signature verification; no downgrade/rollback without signature.
- Size/hash/signature checked before install; partial/corrupt downloads rejected.
- Delta/full packages; delta base version must match exactly.
- Manifest cached with ETag; offline/no-network is a normal state.

## 3. Update flow

```text
check -> download (background, resumable) -> verify -> stage
  -> prompt/auto policy -> apply on restart/next launch -> health check
  -> success or rollback to previous version
```

- Download off the UI thread, bounded, resumable; never block startup.
- Apply atomically (new dir/slot); keep the previous version until health check passes.
- Rollback on launch failure/crash loop; never leave a half-installed app.
- User consent policy is explicit; security updates may be prioritized (the prioritization rule is not specified).
- No update while critical unsaved work is active; prompt/save first.
- Update does not run arbitrary scripts from the manifest.

## 4. Platform mapping

| Platform | Mechanism |
|---|---|
| Windows | signed installer/MSIX or app-managed updater; per-user/per-machine policy |
| macOS | signed/notarized `.app`/pkg; Sparkle-like or app-managed; hardened runtime |
| Linux | AppImage/Flatpak/tarball; sandbox permissions; distribution repo package (which distros are not specified) |
| Android | Play/App Bundle or enterprise MDM; in-app update API |
| iOS | App Store/TestFlight; no side-loaded updater |

- Code signing/notarization is mandatory on every platform whose OS requires it (see the
  platform mapping above for macOS and Windows).
- Installer/portable choice is documented per app.
- Uninstall preserves/removes user data by explicit policy.

## 5. Security

- Verify TLS + signature + hash before executing/installing.
- Pin update roots/keys; rotate keys with a signed transition.
- No update URL from untrusted input; allowlist hosts.
- Least privilege: updater runs with the minimum permissions; no elevation unless the OS install
  step requires it.
- SBOM/notices are shipped with the update; reproducibility documented.
- Crash/telemetry from old versions is version-matched and redacted.

## 6. Testing

- Manifest parsing/signature/hash/channel/platform/arch.
- Resume/corrupt download/partial file/offline.
- Atomic stage/apply/rollback/health check.
- Migration across versions; downgrade policy.
- Signing/notarization checks on clean VMs.
- Crash loop -> rollback; no data loss.
- Headless updater tests with fake transport/filesystem.

## 6.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `UpdateClient` | check/download/verify/stage/apply/rollback | App data migration |
| `UpdateManifest` | channel/version/platform/url/hash/signature/minOs | Release build/signing keys |
| `UpdateStorage` | staging dir, previous version, atomic swap | User documents |
| Platform installer | OS install/notarization/store policy | App update policy |

State machine:

```text
Idle -> Checking -> UpToDate
             \-> Available -> Downloading -> Verifying -> Staged
                                                       -> Applying -> HealthCheck
                                                          -> Healthy (commit)
                                                          -> Rollback (previous)
```

- `Checking/Downloading` run off the UI thread; progress is a resource/state, not a blocking
  modal.
- `Verifying`: TLS + manifest signature + package hash + platform identity; any failure aborts.
- `Staged`: package is complete and immutable; apply only on restart/next launch (section 3) with
  no unsaved work active.
- `HealthCheck`: launch/version/first-frame sentinel; failure/crash loop -> rollback.
- Previous version/data migration backup is retained until health check succeeds.

Manifest/signing:

- Manifest fields are canonicalized and signed; unknown fields rejected or ignored depending on the manifest version (the rule per version is
  not specified).
- Public keys are pinned; key rotation uses a signed transition manifest.
- Delta base version/build id must match; otherwise full package.
- `minOs`/arch/channel mismatch -> no offer.
- HTTPS allowlist; no URL from app content/user input.

Apply/rollback:

- Atomic directory/slot swap or platform installer transaction; never patch in place.
- Preserve user data/config; migration is idempotent and versioned.
- Rollback restores the previous binary and compatible data; incompatible migration is explicit.
- Do not apply while unsaved work is active; save/prompt first.
- If the app crashes before health check, count attempts and roll back after N attempts (G-23 below sets N = 2 failed launches).

Failure/edge cases:

- Offline/partial download -> resume/cancel; no partial install.
- Signature/hash mismatch -> delete package, diagnostic, no install.
- Disk full/permission denied -> abort with actionable error.
- Power loss during swap -> recover previous/complete slot.
- Downgrade -> blocked by default; explicit signed policy only.

Gap-closure decisions:

- **G-23/Q-22**: full package is V1; delta is deferred to v1.1. Manifest signing is Ed25519 with
  pinned keys and signed key rotation. Health check = version/first frame on next launch; two
  failed launches trigger rollback. Consent: check/download may be automatic, applying requires
  an explicit policy (`onRestart`/`prompt`/`manual`). Downgrades are blocked unless explicitly
  signed.

## 7. Deferred/open questions

- Delta format/tooling and signing (planned for v1.1, per G-23 above).
- In-app updater vs platform store per OS.
- Quiet-hours/consent UX details (no decision yet).
- Enterprise/MDM distribution.
- Data migration guarantees across major versions.
