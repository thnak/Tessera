// SPDX-License-Identifier: MIT

// The WebGPU *device* half of the merged studio page's Canvas view (issue #392; the plumbing is P3,
// issue #167). Split out of `view-canvas.js` so neither file passes the repository's source-size limit,
// and because it is a separate responsibility with a separate failure mode: acquiring an adapter and a
// device, wrapping them in the handles the module takes, and configuring the canvas surface. None of it
// draws.
//
// Every failure path returns a string a person can read, because the alternative is the console-only
// "GPUAdapter is null" that tells a designer nothing. The caller renders that string as the view's
// named unsupported state; a blank canvas is never the outcome (docs/design/compiler-wasm.md 8.5).
//
// The handover is explicit for a reason the module cannot avoid: requesting a device is async
// JavaScript, so the Wasm module cannot do it. The page does the dance and calls
// `tessera_webgpu_set_device` once. The C ABI takes Emscripten WebGPU *handles*, not JS objects: a JS
// object coerces to 0, which the module reads as a null handle and answers "no WebGPU device".

/** The canvas format the page configures and the module paints in (wasm_canvas_entry.cpp's format). */
export const CANVAS_FORMAT = 'rgba8unorm';

/** Acquires an adapter and device, or returns `{ error }` with a sentence naming what failed. */
export async function acquireDevice() {
  if (!navigator.gpu) {
    return {
      error:
        'This browser has no navigator.gpu. WebGPU is available in Chrome/Edge 113+, Safari 18+ and ' +
        'Firefox 141+ (on supported hardware). The Scene tab needs no GPU at all.',
    };
  }
  let adapter;
  try {
    adapter = await navigator.gpu.requestAdapter();
  } catch (error) {
    return { error: `requestAdapter() failed: ${String(error)}` };
  }
  if (!adapter) {
    return {
      error:
        'No WebGPU adapter is available. This usually means the GPU or its driver is blocklisted, or ' +
        'the page is in a browser context that forbids GPU access.',
    };
  }
  try {
    const device = await adapter.requestDevice();
    if (!device) {
      return { error: 'requestDevice() returned no device.' };
    }
    return { device, queue: device.queue };
  } catch (error) {
    return { error: `requestDevice() failed: ${String(error)}` };
  }
}

/**
 * Hands `device`/`queue` to `module` and configures `canvas`'s surface.
 *
 * Returns `{ context }` on success or `{ error }`. The module's own return value is three-way --
 * 1 accepted, 0 null handle, -1 wrapping threw -- and each maps to a sentence rather than a number.
 */
export function handOverDevice(module, device, queue, canvas) {
  if (typeof module._tessera_webgpu_set_device !== 'function') {
    return {
      error: 'The module this page loaded cannot accept a WebGPU device (`tessera_webgpu_set_device`).',
    };
  }
  // The module exports the port's handle table for exactly this import.
  if (!module.WebGPU || typeof module.WebGPU.importJsDevice !== 'function') {
    return {
      error:
        'The module this page loaded does not export the WebGPU handle table ' +
        '(`WebGPU.importJsDevice`), so it cannot take the browser device. Rebuild the canvas module ' +
        'with the current link options.',
    };
  }
  const handover = module._tessera_webgpu_set_device(
    module.WebGPU.importJsDevice(device),
    module.WebGPU.importJsQueue(queue),
  );
  if (handover !== 1) {
    return {
      error:
        handover === 0
          ? 'The module received a null WebGPU handle from the page.'
          : 'The module could not wrap the WebGPU device the page handed it.',
    };
  }
  // The canvas surface is the page's to configure (the RHI never owns it): RGBA8 is one of the two
  // formats every WebGPU canvas must support, and it is the format the module paints in. Opaque,
  // because the preview clears to the app background and a transparent canvas would show the page.
  const context = canvas.getContext('webgpu');
  if (!context) {
    return { error: 'The canvas could not create a WebGPU context, so the frame has nowhere to be shown.' };
  }
  context.configure({ device, format: CANVAS_FORMAT, alphaMode: 'opaque' });
  return { context };
}
