# Third-party

Vendored dependencies. Keep each one in its own subdirectory and record its license here.

| Dependency | Version | License | Used for |
|---|---|---|---|
| `stb/stb_image_write.h` | master (fetched 2026-11-07) | Public domain / MIT (dual) | PNG output for the headless Vulkan PoC and goldens |
| `stb/stb_image.h` | master (fetched 2026-11-07) | Public domain / MIT (dual) | PNG/JPEG decode for embedded image assets and goldens |
| `yoga/yoga` | v3.2.2 | MIT | Flexbox layout engine |

Fetched at configure time (pinned by archive hash, see `CMakeLists.txt`): FreeType, HarfBuzz, SheenBidi, libjpeg-turbo 3.0.4 (BSD-3-Clause / IJG, plain C, no SIMD; scaled JPEG decode) and libwebp 1.4.0 (BSD-3-Clause; WebP decode), both for `rendering/image`.
