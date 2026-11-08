# Updates and distribution v1

Status: proposed (Big Step F). Decision: ADR-031. Companion to
[`build-and-packaging.md`](../design/build-and-packaging.md), [`packaging.md`](../guides/packaging.md),
and [`security.md`](../design/security.md).

Desktop apps need a safe update path; mobile apps use store updates but still need versioning,
signing, and rollback discipline.

## 1. Versioning and channels

- SemVer for the app; build metadata includes commit/toolchain/config hash.
- Channels: `stable`, `beta`, `nightly` (optional), per-platform.
- Update manifest is signed; channel/version/platform/arch are explicit.
- Downgrades require an explicit policy/signature; no silent rollback across incompatible data.
- Data/schema migrations run once and are forward-compatible where possible.

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
- User consent policy is explicit; security updates may be prioritized.
- No update while critical unsaved work is active; prompt/save first.
- Update does not run arbitrary scripts from the manifest.

## 4. Platform mapping

| Platform | Mechanism |
|---|---|
| Windows | signed installer/MSIX or app-managed updater; per-user/per-machine policy |
| macOS | signed/notarized `.app`/pkg; Sparkle-like or app-managed; hardened runtime |
| Linux | AppImage/Flatpak/tarball; sandbox permissions; repo package where applicable |
| Android | Play/App Bundle or enterprise MDM; in-app update API |
| iOS | App Store/TestFlight; no side-loaded updater |

- Code signing/notarization is mandatory where the OS requires it.
- Installer/portable choice is documented per app.
- Uninstall preserves/removes user data by explicit policy.

## 5. Security

- Verify TLS + signature + hash before executing/installing.
- Pin update roots/keys; rotate keys with a signed transition.
- No update URL from untrusted input; allowlist hosts.
- Least privilege: updater runs with the minimum permissions; no elevation unless required.
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

## 7. Open questions

- In-app updater vs platform store per OS.
- Delta format/tooling and signing.
- Update consent/quiet-hours policy.
- Rollback and data migration guarantees.
- Enterprise/MDM distribution.
