// SPDX-License-Identifier: MIT
// Emscripten --js-library: the two imports the C++ web host calls (web_exports.cpp). They forward to
// the frame bridge the page installs on Module.tesseraBridge (tessera_web_host.js).
mergeInto(LibraryManager.library, {
  tessera_js_request_frame: function () {
    Module.tesseraBridge.requestFrame();
  },
  // 1 when the page requested a WebGPU device before instantiating the module (boot in
  // tessera_web_host.js). Node and a browser without WebGPU have none, and the app then draws nothing.
  tessera_js_has_gpu_device: function () {
    return Module.preinitializedWebGPUDevice ? 1 : 0;
  },
  tessera_js_wake_after: function (milliseconds) {
    Module.tesseraBridge.wakeAfter(milliseconds);
  },
});
