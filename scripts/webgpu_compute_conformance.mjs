// SPDX-License-Identifier: MIT
//
// Runs the WebGPU compute conformance program (issue #684) against a real GPU under Node, using
// Dawn's `webgpu` bindings (https://www.npmjs.com/package/webgpu) as the WebGPU implementation.
//
//   cmake --preset wasm-webgpu && cmake --build --preset wasm-webgpu --target webgpu_compute_conformance
//   npm install webgpu --prefix <scratch dir> --ignore-scripts      (kept out of the repository)
//   TESSERA_DAWN_DIR=<scratch dir> node scripts/webgpu_compute_conformance.mjs
//
// The same host runs scripts' other Wasm programs: TESSERA_CONFORMANCE_JS names the generated .js (for
// example build/wasm-webgpu/src/rendering/backend/webgpu/webgpu_path_raster_conformance.js, issue #685),
// and arguments after the script name reach the program's main. TESSERA_DAWN_MAX_LIMITS=1 requests the
// adapter's own storage-binding and buffer size limits (a 4K raster needs more than the 128 MiB default).
//
// The Wasm module gets the device through `Module.preinitializedWebGPUDevice`; Node has no
// `navigator.gpu`, so the bindings stand in for it. Exit code is the program's.

import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const dawnDir = process.env.TESSERA_DAWN_DIR;
if (!dawnDir) {
    console.error('set TESSERA_DAWN_DIR to a directory with the "webgpu" npm package installed');
    process.exit(2);
}
const { create, globals } = await import(
    pathToFileURL(path.join(dawnDir, 'node_modules', 'webgpu', 'index.js')).href);
Object.assign(globalThis, globals);
// Extra Dawn flags, comma separated (for example backend=vulkan, enable-dawn-features=disable_robustness).
const gpu = create((process.env.TESSERA_DAWN_FLAGS ?? '').split(',').filter(Boolean));
Object.defineProperty(globalThis, 'navigator', { value: { gpu }, configurable: true });

const adapter = await gpu.requestAdapter();
if (!adapter) {
    console.error('no WebGPU adapter');
    process.exit(2);
}
const info = adapter.info ?? {};
console.log('adapter:', info.description ?? info.vendor ?? 'unknown',
    JSON.stringify({ vendor: info.vendor, architecture: info.architecture, device: info.device,
                     fallback: adapter.isFallbackAdapter ?? info.isFallbackAdapter }));
const requiredLimits = {};
if (process.env.TESSERA_DAWN_MAX_LIMITS) {
    for (const name of ['maxStorageBufferBindingSize', 'maxBufferSize']) {
        requiredLimits[name] = adapter.limits[name];
    }
}
const device = await adapter.requestDevice({ requiredLimits });
device.addEventListener?.('uncapturederror', (event) => {
    console.error('WebGPU error:', event.error?.message ?? event.error);
    process.exitCode = 4;
});

const module = path.resolve(process.env.TESSERA_CONFORMANCE_JS ??
    'build/wasm-webgpu/src/rendering/backend/webgpu/webgpu_compute_conformance.js');
// The generated script keeps its own `var Module` inside the CommonJS wrapper, so a global Module is
// ignored; it exports that object instead. Instantiation is asynchronous, so these are set before main.
const wasmModule = createRequire(import.meta.url)(module);
wasmModule.preinitializedWebGPUDevice = device;
wasmModule.onExit = (code) => {
    process.exitCode = process.exitCode || code;
};
