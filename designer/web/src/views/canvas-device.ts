// SPDX-License-Identifier: MIT

// The WebGPU *device* half of the Canvas view (P3, issue #167; the React host is issue #422).
//
// Kept apart from the draw path because it is a separate responsibility with a separate failure mode:
// acquiring an adapter and a device, wrapping them in the handles the module takes, and configuring the
// canvas surface. None of it draws.
//
// Every failure path returns a string a person can read, because the alternative is the console-only
// "GPUAdapter is null" that tells a designer nothing. The caller renders that string as the view's named
// unsupported state; a blank canvas is never the outcome (docs/design/compiler-wasm.md 8.5).
//
// The handover is explicit for a reason the module cannot avoid: requesting a device is async JavaScript,
// so the Wasm module cannot do it. The page does the dance and calls `tessera_webgpu_set_device` once.
// The C ABI takes Emscripten WebGPU *handles*, not JS objects: a JS object coerces to 0, which the
// module reads as a null handle and answers "no WebGPU device".

/** The `WebGPU` handle table the Emscripten port exports for exactly this handover. */
interface WebGpuHandles {
  importJsDevice(device: GPUDevice): number;
  importJsQueue(queue: GPUQueue): number;
  importJsTextureView(view: GPUTextureView): number;
}

/** The canvas module, once the device half has had its say. */
export interface CanvasModule {
  WebGPU?: WebGpuHandles;
  _tessera_webgpu_set_device(deviceHandle: number, queueHandle: number): number;
  [name: string]: unknown;
}

/** The canvas format the page configures and the module paints in (wasm_canvas_entry.cpp's format). */
export const CANVAS_FORMAT = "rgba8unorm";

/** An acquired device and its queue. */
export interface AcquiredDevice {
  device: GPUDevice;
  queue: GPUQueue;
}

/** Either a device or a sentence naming what failed; exactly one is present. */
export type DeviceResult = ({ device: GPUDevice; queue: GPUQueue; error?: undefined } | { device?: undefined; queue?: undefined; error: string });

/**
 * A read-only counter of the devices this page took, for the browser gates ("close Canvas and see no GPU
 * device taken"): `acquired` is how many were requested, `live` how many have not been lost or destroyed.
 */
function trackDevice(device: GPUDevice, adapter: GPUAdapter): void {
  const holder = globalThis as { __tesseraGpu?: GpuStats };
  const stats = (holder.__tesseraGpu ??= { acquired: 0, live: 0, errors: [], lost: [] });
  stats.acquired += 1;
  stats.live += 1;
  const acquiredAt = performance.now();
  // What a loss on a runner needs to be diagnosable (issue #958): the browser's own reason and message,
  // not only the spec's coarse "unknown", the validation errors that preceded it, and the adapter.
  device.addEventListener("uncapturederror", (event) => {
    if (stats.errors.length < 8) {
      stats.errors.push(String(event.error?.message ?? "uncaptured error"));
    }
  });
  void device.lost.then((info) => {
    stats.live -= 1;
    stats.lost.push({
      reason: String(info.reason),
      message: info.message,
      afterMs: Math.round(performance.now() - acquiredAt),
      adapter: describeAdapter(adapter),
      destroyedByUs: ownDestroys.has(device),
    });
  });
}

/** Devices this page destroyed itself, so a loss report can say "we did that" and not "the runner did". */
const ownDestroys = new WeakSet<GPUDevice>();

/** Destroys a device the page acquired, recording that the loss that follows is the page's own doing. */
export function destroyDevice(device: GPUDevice): void {
  ownDestroys.add(device);
  device.destroy();
}

/** One line naming the adapter and the device limits that matter for texture and buffer size. */
function describeAdapter(adapter: GPUAdapter): string {
  const info = (adapter as { info?: { vendor?: string; architecture?: string; device?: string; description?: string } })
    .info;
  const limits = adapter.limits;
  return (
    `${info?.vendor ?? "?"}/${info?.architecture ?? "?"}/${info?.device ?? "?"} ${info?.description ?? ""} ` +
    `maxTexture2D=${limits.maxTextureDimension2D} maxBuffer=${limits.maxBufferSize}`
  ).trim();
}

/** What `window.__tesseraGpu` holds: counters for the gates plus the loss diagnostics. */
interface GpuStats {
  acquired: number;
  live: number;
  errors: string[];
  lost: { reason: string; message: string; afterMs: number; adapter: string; destroyedByUs: boolean }[];
}

/** The reason and message of the page's last device loss, as one sentence fragment, or `null`. */
export function describeLoss(info: { reason?: string; message: string }): string {
  const message = info.message.trim();
  return message === "" ? info.reason || "unknown" : `${info.reason || "unknown"}: ${message}`;
}

/** Acquires an adapter and device, or returns `{ error }` with a sentence naming what failed. */
export async function acquireDevice(): Promise<DeviceResult> {
  if (!navigator.gpu) {
    return {
      error:
        "This browser has no navigator.gpu. WebGPU is available in Chrome/Edge 113+, Safari 18+ and " +
        "Firefox 141+ (on supported hardware). The Scene tab needs no GPU at all.",
    };
  }
  let adapter: GPUAdapter | null;
  try {
    adapter = await navigator.gpu.requestAdapter();
  } catch (error) {
    return { error: `requestAdapter() failed: ${String(error)}` };
  }
  if (!adapter) {
    return {
      error:
        "No WebGPU adapter is available. This usually means the GPU or its driver is blocklisted, or " +
        "the page is in a browser context that forbids GPU access.",
    };
  }
  try {
    const device = await adapter.requestDevice();
    if (!device) {
      return { error: "requestDevice() returned no device." };
    }
    trackDevice(device, adapter);
    return { device, queue: device.queue };
  } catch (error) {
    return { error: `requestDevice() failed: ${String(error)}` };
  }
}

/**
 * Hands `device`/`queue` to `module` and configures `canvas`'s surface.
 *
 * Returns `{ context }` on success or `{ error }`. The module's own return value is three-way -- 1
 * accepted, 0 null handle, -1 wrapping threw -- and each maps to a sentence rather than a number.
 */
export function handOverDevice(
  module: CanvasModule,
  device: GPUDevice,
  queue: GPUQueue,
  canvas: HTMLCanvasElement,
): { context: GPUCanvasContext; error?: undefined } | { context?: undefined; error: string } {
  if (typeof module._tessera_webgpu_set_device !== "function") {
    return {
      error: "The module this page loaded cannot accept a WebGPU device (`tessera_webgpu_set_device`).",
    };
  }
  // The module exports the port's handle table for exactly this import.
  if (!module.WebGPU || typeof module.WebGPU.importJsDevice !== "function") {
    return {
      error:
        "The module this page loaded does not export the WebGPU handle table " +
        "(`WebGPU.importJsDevice`), so it cannot take the browser device. Rebuild the canvas module " +
        "with the current link options.",
    };
  }
  const handles = module.WebGPU;
  const handover = module._tessera_webgpu_set_device(
    handles.importJsDevice(device),
    handles.importJsQueue(queue),
  );
  if (handover !== 1) {
    return {
      error:
        handover === 0
          ? "The module received a null WebGPU handle from the page."
          : "The module could not wrap the WebGPU device the page handed it.",
    };
  }
  // The canvas surface is the page's to configure (the RHI never owns it): RGBA8 is one of the two
  // formats every WebGPU canvas must support, and it is the format the module paints in. Opaque,
  // because the preview clears to the app background and a transparent canvas would show the page.
  //
  // The cast is the one place the page has to assert a platform type: `lib.dom` types `getContext` for the
  // 2D, WebGL and bitmap contexts only, and `"webgpu"` is not one of them. A browser without WebGPU returns
  // `null` here rather than a wrong object, which is why the null branch below is a real state.
  const context = canvas.getContext("webgpu") as GPUCanvasContext | null;
  if (!context) {
    return { error: "The canvas could not create a WebGPU context, so the frame has nowhere to be shown." };
  }
  context.configure({ device, format: CANVAS_FORMAT, alphaMode: "opaque" });
  return { context };
}
