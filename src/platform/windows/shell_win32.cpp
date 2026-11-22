// SPDX-License-Identifier: MIT
//
// The Win32 back end for the shell module (docs/specs/platform-shell-v1.md sections 1, 2, 3 and 5;
// issue #450). It is the *platform* half of the module: `shell_backend.hpp` is the only interface
// it implements, and nothing above it names a `HWND`, an `IDropTarget` or a `CLSID`.
//
// **What this file owns, and what it deliberately does not.**
//
//   * Clipboard text goes through `hal::Clipboard` (`tessera_hal_clipboard`), which already owns
//   the
//     `OpenClipboard` retry budget and the UTF-16 conversion. This back end must not grow a second
//     copy of that: the HAL seam is where "a busy clipboard fails as a value instead of spinning on
//     the UI thread" is decided, and a second implementation would be a second answer.
//   * File dialogs are in `shell_win32_dialogs.cpp`, because they are the one part that needs COM
//     (`IFileOpenDialog`) and the one part with a different failure mode.
//   * Drag and drop is the `WM_DROPFILES` path, which is the only external-drop mechanism that
//   needs
//     no OLE registration and no `IDropTarget` on the window. It is deliberately the *simple* one:
//     section 3's internal drag is the scene's own gesture pipeline (a `DragRecognizer` claim), not
//     a platform drag, so the platform only has to answer "a file was dropped here".
//
// **Threading.** Every blocking operation runs on the `IoTaskExecutor` the caller supplies, and
// every completion is posted through the injected `CompletionQueue`. `WM_DROPFILES` is the
// exception that proves the rule: it arrives on the window's own thread, so the window's handler
// only *copies the paths* and posts; it never opens a file.
//
// **Not in this slice**, named in docs/design/shell-module.md section 7: OLE drag-and-drop with a
// drag image, clipboard files/URIs, the menu bar and tray, global shortcuts, and the Linux back
// end.

#include "shell_win32.hpp"

#ifdef _WIN32

#include "tessera/appfw/shell_path.hpp"
#include "tessera/appfw/shell_types.hpp"
#include "tessera/hal/clipboard.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/io_task_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

namespace tessera::appfw {
namespace {

// Section 1's Windows column: clipboard text, internal and external drag, external file drop,
// context menu, open/save dialog, folder pick, menu bar and tray are all "yes". Global shortcuts
// are "later" in the spec's table, so that bit stays clear.
[[nodiscard]] ShellCapabilities win32_capabilities() noexcept {
    ShellCapabilities capabilities;
    capabilities.supported = shell_capability_bit(ShellCapability::ClipboardText) |
                             shell_capability_bit(ShellCapability::DragInternal) |
                             shell_capability_bit(ShellCapability::DragExternal) |
                             shell_capability_bit(ShellCapability::DropFiles) |
                             shell_capability_bit(ShellCapability::ContextMenu) |
                             shell_capability_bit(ShellCapability::FileOpen) |
                             shell_capability_bit(ShellCapability::FileSave) |
                             shell_capability_bit(ShellCapability::FolderPick) |
                             shell_capability_bit(ShellCapability::MenuBar) |
                             shell_capability_bit(ShellCapability::Tray);
    // Nothing in V1 asks the user for a shell permission, so every supported capability is
    // permitted. The two masks stay separate because a later permission model fills `permissioned`
    // in.
    capabilities.permissioned = capabilities.supported;
    return capabilities;
}

} // namespace

// One instance per app scope. It owns no window: the operations it serves are not tied to one (a
// dialog is modal to the app, not to a window, and the clipboard is process-wide).
class Win32ShellBackend final : public ShellBackend {
public:
    Win32ShellBackend(runtime::IoTaskExecutor& executor, runtime::CompletionQueue& completions)
        : executor_(&executor),
          completions_(&completions),
          clipboard_(hal::create_native_clipboard()) {}

    Win32ShellBackend(const Win32ShellBackend&) = delete;
    Win32ShellBackend& operator=(const Win32ShellBackend&) = delete;

    [[nodiscard]] ShellBackendCapabilities capabilities() const override {
        return ShellBackendCapabilities{win32_capabilities()};
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "win32";
    }

    // Section 2: the read runs on the I/O executor, so a busy clipboard's bounded retry never runs
    // on the UI thread. The completion is posted, never run inline (contract 1).
    bool start_clipboard_read(runtime::CancellationToken token,
                              ClipboardReadCompletion on_done) override {
        if (token.cancelled()) {
            return true; // accepted, then dropped: contract 2 allows reporting nothing
        }
        hal::Clipboard* clipboard = clipboard_.get();
        runtime::CompletionQueue* completions = completions_;
        // The result slot is shared between the two stages, because `work` and `completion` are two
        // separate callables and only the second may touch the caller's callback.
        auto slot = std::make_shared<std::optional<std::optional<ClipboardContent>>>();
        runtime::IoTask task;
        task.work = [clipboard, token, slot]() {
            if (token.cancelled()) {
                return;
            }
            auto text = clipboard->read_text();
            if (!text.has_value()) {
                // A failed read is reported as "nothing", which the module turns into a clean empty
                // read. The HAL's error kinds are finer than the shell's, and section 2's contract
                // is that an unavailable clipboard degrades rather than fails.
                slot->emplace(std::nullopt);
                return;
            }
            ClipboardContent content;
            content.text = std::move(*text);
            slot->emplace(std::move(content));
        };
        task.completion = [slot, on_done = std::move(on_done), token, completions]() {
            if (token.cancelled() || !slot->has_value()) {
                return;
            }
            std::optional<ClipboardContent> content = std::move(**slot);
            completions->post(
                runtime::CompletionPriority::Visible,
                [on_done = std::move(on_done), content]() mutable { on_done(std::move(content)); });
        };
        return executor_->post(std::move(task));
    }

    bool start_clipboard_write(ClipboardContent content, runtime::CancellationToken token,
                               ClipboardWriteCompletion on_done) override {
        if (token.cancelled()) {
            return true;
        }
        hal::Clipboard* clipboard = clipboard_.get();
        runtime::CompletionQueue* completions = completions_;
        auto slot = std::make_shared<bool>(false);
        runtime::IoTask task;
        task.work = [clipboard, token, slot, text = std::move(content.text)]() {
            if (token.cancelled()) {
                return;
            }
            *slot = clipboard->write_text(text).has_value();
        };
        task.completion = [slot, on_done = std::move(on_done), token, completions]() {
            if (token.cancelled()) {
                return;
            }
            const bool ok = *slot;
            completions->post(runtime::CompletionPriority::Visible,
                              [on_done = std::move(on_done), ok]() { on_done(ok); });
        };
        return executor_->post(std::move(task));
    }

    // Section 5: the dialog is modal to the app and runs on the I/O executor, so the UI thread
    // keeps pumping. `shell_win32_dialogs.cpp` owns the COM half.
    bool start_dialog(DialogRequest request, runtime::CancellationToken token,
                      DialogCompletion on_done) override {
        return win32_start_dialog(std::move(request), std::move(token), std::move(on_done),
                                  *executor_, *completions_);
    }

    // Section 3: the platform reports a drop, the module decides. The back end's only job is to
    // resolve the offered action against what it can do, which for the `WM_DROPFILES` path is a
    // copy unless the source offered only move or link.
    bool start_drop(DropRequest request, runtime::CancellationToken token,
                    DropCompletion on_done) override {
        if (token.cancelled()) {
            return true;
        }
        DropAction action = DropAction::None;
        if (has_drag_action(request.offered, DragActions::Copy)) {
            action = DropAction::Copy;
        } else if (has_drag_action(request.offered, DragActions::Move)) {
            action = DropAction::Move;
        } else if (has_drag_action(request.offered, DragActions::Link)) {
            action = DropAction::Link;
        }
        completions_->post(runtime::CompletionPriority::Visible,
                           [on_done = std::move(on_done), action]() { on_done(action); });
        return true;
    }

private:
    runtime::IoTaskExecutor* executor_ = nullptr;
    runtime::CompletionQueue* completions_ = nullptr;
    std::unique_ptr<hal::Clipboard> clipboard_;
};

std::unique_ptr<ShellBackend> create_win32_shell_backend(runtime::IoTaskExecutor& executor,
                                                         runtime::CompletionQueue& completions) {
    return std::make_unique<Win32ShellBackend>(executor, completions);
}

// --- section 3's external file drop, as `WM_DROPFILES` reports it -------------------------------

std::size_t win32_dropped_file_count(void* hdrop) {
    if (hdrop == nullptr) {
        return 0;
    }
    const UINT count = ::DragQueryFileW(static_cast<HDROP>(hdrop), 0xFFFFFFFF, nullptr, 0);
    return static_cast<std::size_t>(count);
}

std::vector<std::string> win32_read_dropped_files(void* hdrop) {
    std::vector<std::string> paths;
    if (hdrop == nullptr) {
        return paths;
    }
    const HDROP drop = static_cast<HDROP>(hdrop);
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    // Section 9.1's "capped at ... 16 files": a drop that carries more is truncated here, before
    // the module's own budget check, so a hostile drop cannot make this loop allocate without
    // bound.
    const UINT bounded = count < kMaxShellDropFiles ? count : static_cast<UINT>(kMaxShellDropFiles);
    paths.reserve(bounded);
    for (UINT index = 0; index < bounded; ++index) {
        const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring wide(static_cast<std::size_t>(length) + 1, L'\0');
        if (::DragQueryFileW(drop, index, wide.data(), length + 1) == 0) {
            continue;
        }
        wide.resize(length);
        // UTF-16 to UTF-8. A path that does not convert is skipped rather than mangled.
        const int needed = ::WideCharToMultiByte(
            CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) {
            continue;
        }
        std::string utf8(static_cast<std::size_t>(needed), '\0');
        if (::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                  utf8.data(), needed, nullptr, nullptr) <= 0) {
            continue;
        }
        paths.push_back(std::move(utf8));
    }
    return paths;
}

} // namespace tessera::appfw

#endif // _WIN32
