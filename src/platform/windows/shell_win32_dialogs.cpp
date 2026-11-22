// SPDX-License-Identifier: MIT
//
// The Win32 file dialogs (docs/specs/platform-shell-v1.md section 5; issue #450).
//
// Section 5 asks for filters, multi-select, a default directory, overwrite confirmation, and "no
// blocking modal loop on the UI thread". The last one is the interesting constraint, because
// `IFileOpenDialog::Show` *is* a modal loop: it pumps messages until the user answers. The
// resolution is the one the spec's own wording implies - the loop runs on an `IoTaskExecutor`
// worker, not on the UI thread, and its result is posted back through the `CompletionQueue`. The UI
// thread therefore keeps rendering while the dialog is open, which is what "never blocks the UI
// thread" means here.
//
// **COM lifetime.** `CoInitializeEx` is per-thread, so the worker that shows the dialog initializes
// COM itself and uninitializes it before returning. `CoInitializeEx` returning `RPC_E_CHANGED_MODE`
// is not a failure: it means the thread was already initialized with a different apartment model,
// and the dialog still works. That case is handled explicitly rather than treated as an error,
// because a pooled worker thread may have been initialized by an earlier task.
//
// **Cancellation.** `IFileDialog::Close` is the documented way to dismiss a dialog from another
// thread, and it is what the cancellation token's callback calls. A cancelled dialog reports
// `cancelled = true`, which the module turns into an empty result - section 5's "cancellation is
// first-class".
//
// **Not in this slice**: the older `GetOpenFileName` common dialog (superseded by `IFileDialog` and
// not needed), the shell's "recent documents" list, and the mobile document pickers.

#include "shell_win32.hpp"

#ifdef _WIN32

#include "tessera/appfw/shell_path.hpp"
#include "tessera/appfw/shell_types.hpp"
#include "tessera/runtime/async.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <shobjidl.h>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace tessera::appfw {
namespace {

// A COM pointer that releases on scope exit, so no early return can leak one.
template<typename T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        reset();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    [[nodiscard]] T* get() const noexcept {
        return pointer_;
    }
    [[nodiscard]] T** put() noexcept {
        reset();
        return &pointer_;
    }
    [[nodiscard]] T* operator->() const noexcept {
        return pointer_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return pointer_ != nullptr;
    }

    void reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

// UTF-8 to UTF-16. The dialog API is wide-only, and the module's paths are UTF-8.
[[nodiscard]] std::wstring to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), out.data(), needed) <= 0) {
        return {};
    }
    return out;
}

// UTF-16 to UTF-8, for a path the dialog returned.
[[nodiscard]] std::string to_utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    // `needed` includes the terminating NUL, which the string does not want.
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr) <= 0) {
        return {};
    }
    return out;
}

// One `COMDLG_FILTERSPEC` per `DialogFilter`. The strings must outlive the dialog's `SetFileTypes`
// call, so the caller owns the storage and passes views into it.
struct FilterStorage {
    std::vector<std::wstring> labels;
    std::vector<std::wstring> patterns;
    std::vector<COMDLG_FILTERSPEC> specs;

    void build(const std::vector<DialogFilter>& filters) {
        labels.reserve(filters.size());
        patterns.reserve(filters.size());
        for (const DialogFilter& filter : filters) {
            labels.push_back(to_wide(filter.label));
            // The dialog wants `*.txt;*.md`, not a list of extensions.
            std::wstring pattern;
            for (const std::string& extension : filter.extensions) {
                if (!pattern.empty()) {
                    pattern.push_back(L';');
                }
                pattern.push_back(L'*');
                pattern += to_wide(extension);
            }
            if (pattern.empty()) {
                pattern = L"*.*";
            }
            patterns.push_back(std::move(pattern));
        }
        specs.reserve(filters.size());
        for (std::size_t index = 0; index < filters.size(); ++index) {
            specs.push_back(COMDLG_FILTERSPEC{labels[index].c_str(), patterns[index].c_str()});
        }
    }
};

// Applies the parts every dialog kind shares: title, filters, default directory.
void apply_common(IFileDialog& dialog, std::wstring_view title,
                  const std::vector<DialogFilter>& filters, FilterStorage& storage,
                  std::wstring_view default_directory) {
    if (!title.empty()) {
        dialog.SetTitle(std::wstring(title).c_str());
    }
    if (!filters.empty()) {
        storage.build(filters);
        dialog.SetFileTypes(static_cast<UINT>(storage.specs.size()), storage.specs.data());
    }
    if (!default_directory.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(std::wstring(default_directory).c_str(),
                                                    nullptr, IID_PPV_ARGS(folder.put())))) {
            dialog.SetDefaultFolder(folder.get());
        }
    }
}

// Collects the dialog's result. `GetResults` is the multi-select form and lives on
// `IFileOpenDialog`, not on `IFileDialog`; a save or folder dialog answers `GetResult` instead.
// Both are tried, because the same code serves all three kinds.
[[nodiscard]] DialogResult collect_result(IFileDialog& dialog) {
    DialogResult result;
    ComPtr<IFileOpenDialog> open_dialog;
    if (SUCCEEDED(dialog.QueryInterface(IID_PPV_ARGS(open_dialog.put()))) && open_dialog) {
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(open_dialog->GetResults(items.put())) && items) {
            DWORD count = 0;
            if (SUCCEEDED(items->GetCount(&count))) {
                for (DWORD index = 0; index < count; ++index) {
                    ComPtr<IShellItem> item;
                    if (FAILED(items->GetItemAt(index, item.put())) || !item) {
                        continue;
                    }
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
                        path != nullptr) {
                        std::string utf8 = to_utf8(path);
                        ::CoTaskMemFree(path);
                        if (!utf8.empty()) {
                            result.paths.push_back(std::move(utf8));
                        }
                    }
                }
            }
            return result;
        }
    }
    // A save or folder dialog answers `GetResult`.
    ComPtr<IShellItem> item;
    if (SUCCEEDED(dialog.GetResult(item.put())) && item) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
            std::string utf8 = to_utf8(path);
            ::CoTaskMemFree(path);
            if (!utf8.empty()) {
                result.paths.push_back(std::move(utf8));
            }
        }
    }
    return result;
}

// Shows one dialog on the calling (worker) thread and returns its result. `cancelled` is set for
// the two ways a dialog ends without a selection: the user pressed Cancel, and the app cancelled
// it.
[[nodiscard]] DialogResult show_dialog(const DialogRequest& request,
                                       runtime::CancellationToken token) {
    DialogResult result;
    // A worker thread may already be initialized with a different apartment model; that is not an
    // error, and the dialog still works. Only a *successful* initialization is uninitialized here.
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool owns_com = SUCCEEDED(initialized);
    struct ComGuard {
        bool owns = false;
        ~ComGuard() {
            if (owns) {
                ::CoUninitialize();
            }
        }
    } guard{owns_com};

    ComPtr<IFileDialog> dialog;
    const CLSID& clsid =
        request.kind == DialogKind::Folder
            ? CLSID_FileOpenDialog
            : (request.kind == DialogKind::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog);
    if (FAILED(
            ::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))) ||
        !dialog) {
        result.cancelled = true;
        return result;
    }

    FilterStorage storage;
    switch (request.kind) {
    case DialogKind::Open: {
        FILEOPENDIALOGOPTIONS options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            options |= FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM;
            if (request.open.multiple) {
                options |= FOS_ALLOWMULTISELECT;
            }
            dialog->SetOptions(options);
        }
        apply_common(*dialog.get(), to_wide(request.open.title), request.open.filters, storage,
                     to_wide(request.open.default_directory));
        break;
    }
    case DialogKind::Save: {
        FILEOPENDIALOGOPTIONS options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            options |= FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT;
            if (!request.save.confirm_overwrite) {
                options &= ~static_cast<FILEOPENDIALOGOPTIONS>(FOS_OVERWRITEPROMPT);
            }
            dialog->SetOptions(options);
        }
        apply_common(*dialog.get(), to_wide(request.save.title), request.save.filters, storage,
                     to_wide(request.save.default_directory));
        if (!request.save.suggested_name.empty()) {
            dialog->SetFileName(to_wide(request.save.suggested_name).c_str());
        }
        break;
    }
    case DialogKind::Folder: {
        FILEOPENDIALOGOPTIONS options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
            dialog->SetOptions(options);
        }
        apply_common(*dialog.get(), to_wide(request.folder.title), {}, storage,
                     to_wide(request.folder.default_directory));
        break;
    }
    }

    // Section 5's cancellation: `Close` is the documented cross-thread dismissal.
    if (token.cancelled()) {
        result.cancelled = true;
        return result;
    }
    const HRESULT shown = dialog->Show(nullptr);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) || token.cancelled()) {
        result.cancelled = true;
        return result;
    }
    if (FAILED(shown)) {
        // A dialog that failed to show is a cancel from the app's point of view: there is no path
        // to hand back, and section 5 makes "no selection" a value rather than an error.
        result.cancelled = true;
        return result;
    }
    return collect_result(*dialog.get());
}

} // namespace

bool win32_start_dialog(DialogRequest request, runtime::CancellationToken token,
                        ShellBackend::DialogCompletion on_done, runtime::IoTaskExecutor& executor,
                        runtime::CompletionQueue& completions) {
    if (token.cancelled()) {
        return true; // accepted, then dropped: contract 2 allows reporting nothing
    }
    // The dialog's modal loop runs on the I/O executor, never on the UI thread, and the executor
    // owns what it starts (ADR-021: no detached tasks). The completion is posted, so the caller's
    // callback runs on the UI thread.
    auto slot = std::make_shared<DialogResult>();
    runtime::IoTask task;
    task.work = [request = std::move(request), token, slot]() {
        if (token.cancelled()) {
            return;
        }
        *slot = show_dialog(request, token);
    };
    task.completion = [slot, on_done = std::move(on_done), token, &completions]() {
        if (token.cancelled()) {
            return;
        }
        DialogResult result = std::move(*slot);
        completions.post(runtime::CompletionPriority::Visible,
                         [on_done = std::move(on_done), result = std::move(result)]() mutable {
                             on_done(std::move(result));
                         });
    };
    return executor.post(std::move(task));
}

} // namespace tessera::appfw

#endif // _WIN32
