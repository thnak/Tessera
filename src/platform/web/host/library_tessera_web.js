// SPDX-License-Identifier: MIT
// Emscripten --js-library: the two imports the C++ web host calls (web_exports.cpp). They forward to
// the frame bridge the page installs on Module.tesseraBridge (tessera_web_host.js).
mergeInto(LibraryManager.library, {
  tessera_js_request_frame: function () {
    Module.tesseraBridge.requestFrame();
  },
  tessera_js_wake_after: function (milliseconds) {
    Module.tesseraBridge.wakeAfter(milliseconds);
  },
});
