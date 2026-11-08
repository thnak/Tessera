# ADR-012: Static linking is a hard requirement

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-001](ADR-001-vulkan-1-3-baseline.md) (Vulkan 1.3 baseline), [ADR-002](ADR-002-webgpu-dawn-for-wasm.md) (WebGPU/Dawn for WebAssembly), [ADR-004](ADR-004-buy-vector-path-rasterization.md) (vector path rasterization), [ADR-010](ADR-010-macos-via-moltenvk-deferred.md) (macOS via MoltenVK, deferred from the first release)
- Supersedes: —

## Context

Deployment target: a Tessera application should ship as a **single self-contained
executable**, without a bundle of third-party DLLs/`.so` files. Static linking simplifies
distribution, removes DLL-hell, and keeps startup deterministic.

Static linking is not just a build flag; it interacts with:

- license obligations (LGPL static linking requires relinkable object files; GPL static
  linking relicenses the whole application)
- the C/C++ runtime (CRT (C runtime) duplication, ABI (application binary interface) boundaries)
- ICU (International Components for Unicode) data packaging
- platform and GPU driver architecture (Vulkan loader and ICDs (installable client driver, the Vulkan driver plug-in))
- accessibility libraries (AT-SPI/glib are LGPL)

## Decision

1. **Static link Tessera and all third-party C/C++ libraries** into the application
   executable. `BUILD_SHARED_LIBS=OFF`.
2. **System components remain dynamic** and are explicitly outside the "static" promise:
   - the Vulkan loader (`vulkan-1.dll` / `libvulkan.so.1`) and GPU ICDs/drivers
   - OS frameworks: Win32/User32/GDI, X11/Wayland, Cocoa/AppKit, Android system libraries
   - glibc on Linux (a fully static glibc is not required and is problematic with NSS (glibc Name Service Switch) and
     `dlopen`; use musl if a fully static Linux binary is ever required)
3. **No LGPL/GPL in statically linked code.**
   - LGPL static linking requires providing relinkable object files or an equivalent
     mechanism and permitting reverse engineering for debugging. That is incompatible with a
     closed-source single binary.
   - GPL static linking would relicense the entire application.
   - AT-SPI/libatspi/glib (LGPL-2.1) must **not** be linked. Linux accessibility must go
     through the D-Bus protocol or an optional runtime-only integration.
   - libdbus (AFL-2.1/GPL-2.0+) must **not** be linked; use the D-Bus protocol/system service.
4. **License notices must be embedded or shipped**: a `THIRD_PARTY_NOTICES.txt` plus an
   About/licenses UI. Apache-2.0 `NOTICE`, MIT/BSD copyright, OFL font licenses, FreeType FTL,
   and Unicode/ICU notices must be preserved.
5. **ICU static data**: build with `--with-data-packaging=static` or embed the data and call
   `udata_setCommonData`. No external `icudt*.dat` file.
6. **Vulkan loader caveat**: the loader may be statically linked from source (Apache-2.0), but
   it still loads GPU ICDs dynamically. A zero-DLL executable is not achievable with Vulkan on
   Windows; the GPU driver/ICD is a system component.
7. **No plugin ABI in V1.** A static link model precludes a stable dynamic plugin boundary;
   plugins are out of scope until a concrete requirement appears.
8. **CI (continuous integration) verifies the link graph**: `dumpbin /dependents` (Windows), `ldd`/`readelf -d`
   (Linux), `otool -L` (macOS). Unexpected third-party dynamic libraries fail the build.

## Consequences

### Positive

- Single executable distribution; no third-party DLL/`.so` bundle.
- Deterministic startup and no DLL version conflicts.
- The dependency set is forced to stay permissively licensed.

### Negative / risks

- Larger binaries and longer link times.
- CRT/ABI constraints; mixing static and dynamic CRT requires care.
- No dynamic plugin system.
- LGPL accessibility libraries cannot be used directly; Linux accessibility needs a
  protocol-level or dynamic system-only approach.
- ICU data must be embedded.
- Vulkan loader and GPU ICDs remain dynamic; "static" cannot mean "zero DLLs".

## Alternatives considered

- **Dynamic linking for everything.** Rejected: contradicts the deployment requirement.
- **Hybrid (static core, dynamic optional modules).** Not adopted: it may be useful for accessibility or
  codecs, but it is not part of [V1](../docs/reference/glossary.md) and no later release is named.
- **Ignore LGPL obligations by linking anyway.** Rejected: not legally safe.
- **Force a fully static Linux binary with glibc.** Rejected for V1: NSS/`dlopen` problems and
  GPU drivers still dynamic; musl is the option if this becomes a hard requirement.

## Follow-up

- Add `TESSERA_STATIC_RUNTIME` build option (`/MT`, `-static-libstdc++ -static-libgcc`).
- Add the CI dependency allowlist check.
- Add the notices generator and an About/licenses surface.
- Build ICU with static data packaging when the text stack lands.
- Revisit the hybrid accessibility path in [Big Step F](../AGENTS.md#big-steps-group-work-dont-micro-step) (production breadth).
