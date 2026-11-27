// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::rhi {

// Semantic version of the public RHI contract in include/tessera/rendering/rhi/** (ADR-049).
// Bump major for breaking changes, minor for additive compatible changes. Backends must
// declare the version they implement (static_assert in backend sources) and report it via
// Device::capabilities().
// 1.1 (2026-11-13): additive `CommandList::copy_target_region` for the isolated-layer seam
// (issue #242, docs/design/gpu/offscreen-layers.md).
// 1.2 (2026-11-13): additive swapchain composite-alpha request (`SwapchainDesc::per_pixel_alpha`)
// and the resolved-mode query (`Swapchain::alpha_mode`/`supports_per_pixel_alpha`), glass-v1.md
// section 6 (issue #243).
// 1.3 (2026-11-13): additive sampled view of an offscreen render target
// (`CommandList::bind_sampled_target`, `RenderTargetDesc::filter`/`address`), the capability the
// glass composite draws with (issue #254, docs/design/gpu/offscreen-layers.md section 5).
// 1.4 (2026-11-14): additive `DeviceDesc::surface` (`SurfacePlatform`) selecting the presentation
// surface family, so a build that can present through both Xlib and Wayland picks the one the HAL
// window belongs to (issue #155, platform(linux) Wayland window + Vulkan surface).
// 1.5 (2026-11-18): additive `BlendMode::DualSource` (per-channel coverage, `src0 * src1 + dst *
// (1 - src1)`) and the capability that gates it, `Device::supports_dual_source_blend()`. The
// capability is pure virtual because a backend that cannot answer it cannot honour the contract;
// the fallback is grayscale coverage, so no frame changes (issue #471 slice 1).
// 1.6 (2026-11-18): additive `TextureDesc::levels` / `Texture::levels()` (a mip chain built from
// the uploaded base level at creation) and the capability that gates it,
// `Device::supports_texture_mipmaps(format)`; pure virtual so every backend answers (issue #491).
// 1.7 (2026-11-18): additive `CommandList::bind_texture_array` (one descriptor set holding an array
// of sampled images a shader indexes per vertex) and the capability that gates it,
// `Device::texture_array_capacity()`. The default is 0, which is the one-set-per-texture path every
// existing caller keeps (issue #480 part 2).
// 1.8 (2026-11-19): additive `Backend::Metal`, the native macOS/iOS backend (ADR-053). No existing
// enumerator changes value and no interface member changes, so a backend that does not implement
// Metal is unaffected; the enumerator exists so a Metal device can report itself honestly instead
// of borrowing another backend's name.
// 1.9 (2026-11-21): additive `Texture::write_region` and `TextureRegion` (one rectangle of an
// existing texture), so an atlas page whose glyph changed a few texels uploads those and not the
// whole page (issue #681). Pure virtual so every backend answers.
// 1.10 (2026-11-21): additive `SurfacePlatform::Android`, the presentation family for
// `VK_KHR_android_surface`. No existing enumerator changes value and no interface member changes,
// so a backend that never presents to an ANativeWindow is unaffected; the enumerator exists so an
// Android build can name its family instead of resolving to `NoPlatform` (issue #189).
// 1.11 (2026-11-22): every RHI `enum class` names an explicit `std::uint8_t` base type (issue
// #734). This is an ABI change, not a source one: no enumerator changes value, no member is added
// or removed, and every existing caller compiles unchanged, but `sizeof` of the enums and of the
// structs holding them shrinks (`RenderTargetDesc` 48 -> 40, `DeviceDesc` 16 -> 12). A backend
// compiled against 1.10 and linked against 1.11 headers would disagree about those layouts, so the
// minor moves even though the source contract is compatible.
// 1.12 (2026-11-22): additive `BufferUsage::Storage`, `CommandList::bind_storage_buffer`,
// `PipelineDesc::storage_buffer` and the capability that gates them,
// `Device::supports_storage_buffers()` (ADR-068 slice 1, issue #669). The enumerator is appended so
// no existing value changes; the capability is pure virtual so every backend answers. A backend
// answering false keeps the push-constant clip path and no frame changes.
// 1.13 (2026-11-22): documented the color-space contract the D2 policy was missing and moved
// presentation to UNORM (issue #791). No enumerator or member changes and every caller compiles
// unchanged: `TextureFormat`'s `*Srgb` values keep their meaning, but the header now states which
// side of the transfer function the pipeline is on, and a backend's surface-format choice changes
// accordingly. Behaviour-compatible for every caller that reads `Swapchain::format()`; the minor
// moves because the documented contract changed.
// 1.14 (2026-11-22): additive `MemoryCategory`, `MemoryCategoryStats`, `TextureDesc::category` and
// `DeviceMemoryStats::categories` (issue #703). memory-model-v1 section 4 asks for explicit
// per-category GPU budgets, and a budget needs a category to attach to: the allocator's totals say
// how much the device holds, not what it holds it for. The new fields are appended with defaults
// (`Other`, all-zero), so every existing caller compiles and behaves unchanged and a backend that
// does not fill the categories reports zeros rather than a wrong attribution. The minor moves
// because `DeviceMemoryStats` grew, which is an ABI change for anything that copies it by value.
// 1.15 (2026-11-22): additive `Pipeline::declares_storage_buffer()`, so a caller can ask whether
// the pipeline it is about to draw with reads a storage buffer before binding one (ADR-068 slice 3,
// issue #671). The default is false, which is the answer for every pipeline created without
// `PipelineDesc::storage_buffer`, so no existing backend or caller changes.
inline constexpr std::uint32_t kRhiVersionMajor = 1;
// 1.16 (2026-11-24): additive compute (issue #683, ADR-071):
// `ComputePipelineDesc`/`ComputePipeline`, `BufferDesc::gpu_read_write`,
// `CommandList::bind_compute_pipeline`/`bind_compute_buffer`/
// `compute_push_constants`/`dispatch`/`compute_barrier`/`copy_buffer` and the capability that gates
// them, `Device::supports_compute()`. Every new virtual has a default (false / throws), so no
// existing backend or caller changes; a backend answering false keeps its CPU path.
// 1.17 (2026-11-24): additive `CommandList::copy_buffer_to_texture`, `BufferTextureCopy`,
// `kTextureCopyRowAlignment`/`aligned_texture_copy_pitch`/`checked_buffer_texture_copy`
// (texture_region.hpp) and the capability that gates them, `Device::supports_buffer_texture_copy()`
// (ADR-072): the handoff from a compute pass's storage buffer to a texture a draw samples. The
// command defaults to throwing and the capability to false, so no existing backend or caller
// changes.
// 1.18 (2026-11-26): additive compute completion (issue #684, ADR-071): `ComputeCapabilities` and
// `Device::compute_capabilities()` (workgroup size/count limits, shared memory, optional subgroup
// size, indirect dispatch, buffer atomics) and `CommandList::dispatch_indirect`, with the null
// backend able to record compute (opt-in `create_null_compute_device`). The new virtuals default to
// zero/false/throws, so no existing backend or caller changes.
// 1.19 (2026-11-27): additive `BlendMode::DestinationIn` (`dst * src.a` on all four channels), the
// mask step of an isolated layer (issue #617, docs/design/gpu/offscreen-layers.md section 5.2).
// The enumerator is appended so no existing value changes; Vulkan, WebGPU, Metal and the null
// backend all express it with fixed blend factors, so there is no capability to query.
// 1.20 (2026-11-27): additive `Device::supports_analytic_gradients()` (ADR-076): the device's
// batched2d program evaluates a gradient per pixel from the frame record buffer, so one gradient
// layer is one quad. Not pure virtual and false by default, so no existing backend or caller
// changes; a backend answering false keeps the banded gradient fallback.
inline constexpr std::uint32_t kRhiVersionMinor = 20;
inline constexpr std::uint32_t kRhiVersion = (kRhiVersionMajor << 16) | kRhiVersionMinor;

} // namespace tessera::rendering::rhi
