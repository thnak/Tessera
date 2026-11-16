// SPDX-License-Identifier: MIT

// The browser entry point of the WebGPU canvas preview (issue #167, P3 of
// docs/design/reference-equantic-ui.md section 7.4).
//
// This module is the *canvas* half of the preview. `wasm_scene_entry.cpp` (the P2 module) answers
// "what is the scene" as JSON on the null RHI; this one answers "draw it", against the WebGPU
// device the page created:
//
//   int tessera_webgpu_set_device(void* device, void* queue)
//       Hands the module the `WGPUDevice`/`WGPUQueue` handles for the device the page requested.
//       They are Emscripten handles from `WebGPU.importJsDevice` / `importJsQueue`, never JS
//       objects (an object coerces to 0 and reads as a null handle). Returns 1 on success, 0 for
//       a null handle, -1 when wrapping threw. Until it succeeds every frame call reports "no
//       device" instead of throwing, which is what the page's unsupported path shows.
//   const char* tessera_canvas_frame(source, file, component, width, height, dpi)
//       Runs the same `prepare_scene()` + `scene_frame_paint()` path the native CLI renders, into
//       an offscreen WebGPU target of `width` x `height`, and returns a JSON report of what it
//       recorded
//       (`canvas_report.cpp`). The pixels stay on the GPU: a browser cannot map a buffer
//       synchronously, so `pixelsAvailable` is false and the pixel comparison is a browser lane's
//       job, not a claim this module makes.
//   void tessera_free(const char* text)
//       Releases a report returned above.
//
// **Why the entry exists only under Emscripten.** It includes `tessera/rendering/webgpu/
// webgpu_device.hpp`, and `tessera_webgpu` is built only for Emscripten or `TESSERA_BUILD_WEBGPU`.
// The half that *can* be checked on a host lane - `scene_frame.cpp` (the renderer) and
// `canvas_report.cpp` (the ABI's payload) - is compiled there as OBJECT libraries, so the code this
// file calls is type-checked on every push even though the file itself is not.
//
// **Verified status.** The native half is proven locally (the same driver through Vulkan and through
// the null backend). The device handover was first written without an Emscripten toolchain and was
// wrong (it passed JS objects as handles); it is now built with emsdk 7.0.0 and exercised in a real
// browser. Pixel comparison against the lavapipe golden is still a browser-lane job.

#include "canvas_report.hpp"
#include "scene_frame.hpp"
#include "tessera/rendering/webgpu/webgpu_device.hpp"

#include <cstdlib>
#include <cstring>
#include <emscripten/emscripten.h>
#include <memory>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace rhi = tessera::rendering::rhi;
namespace webgpu = tessera::rendering::webgpu;

// The device the page handed over. One per module instantiation: the page requests an adapter and a
// device once and reuses them across edits, so the module keeps the wrapper for its lifetime. A
// second handover replaces it (a page that lost and re-acquired the device).
std::unique_ptr<rhi::Device> g_device;

char* duplicate_out(std::string_view text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return out;
}

} // namespace

extern "C" {

// `device` and `queue` are Emscripten WebGPU handles (integers from `WebGPU.importJsDevice` and
// `WebGPU.importJsQueue`), not JS objects: a JS object passed here is coerced to 0 and would look
// like a null handle. Returns 1 when the device was wrapped, 0 for a null handle, -1 when wrapping
// threw, so the page can say why instead of showing a bare "no device" later.
EMSCRIPTEN_KEEPALIVE int tessera_webgpu_set_device(void* device, void* queue) {
    g_device.reset();
    if (device == nullptr || queue == nullptr) {
        return 0;
    }
    try {
        g_device = webgpu::create_device(device, queue);
    } catch (...) {
        // A module that cannot wrap the handles keeps answering "no device" rather than aborting
        // the page; the page then shows its unsupported state.
        g_device.reset();
        return -1;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_frame(const char* source, const char* file,
                                                      const char* component, int width, int height,
                                                      float dpi) {
    const auto extent = rhi::Extent2D{static_cast<std::uint32_t>(width > 0 ? width : 0),
                                      static_cast<std::uint32_t>(height > 0 ? height : 0)};
    if (g_device == nullptr) {
        cli::SceneFrameResult none;
        none.error = "no WebGPU device: call tessera_webgpu_set_device first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }

    cli::ScenePreviewOptions options;
    if (file != nullptr && file[0] != '\0') {
        options.file = file;
    }
    // The layout viewport is the page's CSS box, not the device-pixel extent: the preview has one
    // viewport in this slice, exactly as the P2 module does, and `dpi` is what scales it up.
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = dpi > 0.0f ? dpi : 1.0f;
    options.salt = "preview";
    const std::string_view name =
        component != nullptr ? std::string_view(component) : std::string_view();
    if (source == nullptr) {
        cli::SceneFrameResult none;
        none.error = "no source";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }

    const cli::SceneFrameResult result =
        cli::scene_frame_render(source, options, *g_device, extent, name);
    return duplicate_out(cli::canvas_report_json(result, extent.width, extent.height, true));
}

// The matching release, same convention as the diagnostics and scene modules.
EMSCRIPTEN_KEEPALIVE void tessera_free(const char* text) {
    std::free(const_cast<char*>(text));
}

} // extern "C"
