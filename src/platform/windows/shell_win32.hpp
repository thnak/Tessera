// SPDX-License-Identifier: MIT
//
// The Win32 shell back end's factory and the one entry point the window's message loop needs
// (docs/specs/platform-shell-v1.md sections 3 and 5; issue #450).
//
// This header is *private to the platform directory*: it is not installed, it is not part of the
// public surface, and nothing above `src/platform/windows` includes it. The public seam is
// `tessera/appfw/shell_backend.hpp`, which names no platform type - that separation is what lets
// the contract suite run on every lane.

#pragma once

#ifdef _WIN32

#include "tessera/appfw/shell_backend.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/io_task_executor.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tessera::appfw {

// The dialog half, defined in `shell_win32_dialogs.cpp`. It is a free function rather than a member
// so `shell_win32.cpp` does not have to include `<shobjidl.h>` and the COM headers.
//
// `executor` is where the dialog's modal loop runs. It is a parameter rather than a thread this
// file starts, because ADR-021 forbids a detached task: the executor owns what it starts, and its
// completion is posted through `completions`.
[[nodiscard]] bool win32_start_dialog(DialogRequest request, runtime::CancellationToken token,
                                      ShellBackend::DialogCompletion on_done,
                                      runtime::IoTaskExecutor& executor,
                                      runtime::CompletionQueue& completions);

// The back end for this build. `executor` is where a blocking clipboard call runs and `completions`
// is where its result is posted; both must outlive the returned object.
[[nodiscard]] std::unique_ptr<ShellBackend>
create_win32_shell_backend(runtime::IoTaskExecutor& executor,
                           runtime::CompletionQueue& completions);

// Section 3's external file drop, as the window's `WM_DROPFILES` handler sees it.
//
// The handler must call this with the `HDROP` it was given *before* `DragFinish`, because the paths
// are read here. It returns the paths the drop carried, already bounded by `kMaxShellDropFiles`,
// and an empty vector when the drop carried nothing usable. It never opens a file: reading the
// paths is the whole operation, and the module validates them before a target sees one.
//
// This is a free function taking a `void*` rather than an `HDROP` so the header stays free of
// `<windows.h>`; the implementation casts it back.
[[nodiscard]] std::vector<std::string> win32_read_dropped_files(void* hdrop);

// The number of files a `WM_DROPFILES` drop carried, without reading the paths. A window uses it to
// decide whether the drop is worth resolving at all.
[[nodiscard]] std::size_t win32_dropped_file_count(void* hdrop);

} // namespace tessera::appfw

#endif // _WIN32
