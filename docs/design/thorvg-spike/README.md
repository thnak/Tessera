# ThorVG spike and path/asset notes

Status: the ThorVG integration spike (task-12, decision D1) is complete and recommends **Go**;
the path rasterizer, texture cache, SVG decode and decoded-document cache slices followed and
are implemented. This folder is the design record for the whole path/asset workstream
(`rendering::PathRasterizer` -> `PathTextureCache` -> `SvgDocumentCache` -> embedded assets).

The project owner approved D1 (ThorVG first) and D2 (sRGB-encoded formats) on 2026-11-09; the
plan is [`../rendering-production-plan.md`](../rendering-production-plan.md).

## Parts

- [Setup and pin](01-setup-and-pin.md) — Why ThorVG was chosen, the exact v1.1.3 pin and how the meson-only upstream is wrapped.
- [Path model and rasterizer seam](02-path-model-and-rasterizer.md) — The PathRasterizer seam, its small path model, the ThorVG mapping and determinism.
- [Measurements and Vulkan viability](03-measurements.md) — Build time, size, startup, memory, determinism evidence and the CPU-only path.
- [Layering, scope and the Go decision](04-layering-scope-and-decision.md) — Layering/warning policy, what the spike deliberately did not do, and the recommendation.
- [Reproducing the spike](05-reproducing.md) — Commands to rebuild the spike, regenerate the golden and run its CTest.
- [Texture cache and upload path](06-texture-cache.md) — PathTextureCache: key contract, LRU bounds, D7 device-loss handling and D2 format rule.
- [SVG decode into path geometry](07-svg-decode.md) — load_svg / SvgDocument: supported subset, reject-vs-ignore rule, bounds and goldens.
- [Decoded-document cache and document textures](08-document-cache.md) — SvgDocumentCache and acquire_svg_texture: content key, dual budgets, batching rule.
- [Embedded asset bundles](09-assets.md) — AssetBundle, tessera_asset_gen/`tessera_embed_assets`, manifest names and memory cost.

## Related

- [`dependencies.md`](../../specs/dependencies.md#1-gpu-and-rendering)
- [`THIRD_PARTY_NOTICES.md`](../../../THIRD_PARTY_NOTICES.md)
- [ADR-004](../../../decisions/ADR-004-buy-vector-path-rasterization.md)
- [ADR-012](../../../decisions/ADR-012-static-linking.md)
- [ADR-013](../../../decisions/ADR-013-project-license-mit.md)
