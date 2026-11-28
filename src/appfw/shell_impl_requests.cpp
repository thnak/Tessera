// SPDX-License-Identifier: MIT
//
// The request half of `ShellServiceImpl` (issue #450): the three operations that reach the back end
// - clipboard, dialogs and drag and drop. It is a separate TU from `shell_impl.cpp` because the two
// halves answer different questions: that file owns the state (capabilities, the one-at-a-time
// gate, handles, the diagnostics ring) and this one owns the per-operation policy (bounds, the
// secure-field rule, path validation, the drop budget) plus the completion plumbing.
//
// Every completion here follows the same three rules, which is why they are worth stating once:
//
//   1. the back end's callback runs on the thread that drains the completion queue, so it may touch
//      service state but must not run the caller's callback inline;
//   2. the caller's callback is posted, so it runs on the UI thread and never on the resolving
//   stack;
//   3. a `weak_ptr` to the impl is checked first, so a service destroyed while an operation is in
//      flight does not leave a callback pointing at freed state.

#include "shell_impl.hpp"
#include "tessera/appfw/shell_path.hpp"

#include <utility>

namespace tessera::appfw::detail {
// --- clipboard ---------------------------------------------------------------------------------

std::expected<void, ShellError>
ShellServiceImpl::submit_clipboard_read(bool secure, ShellService::ClipboardCallback on_done) {
    if (secure && !config_.allow_secure_clipboard) {
        ShellError error{ShellErrorKind::Denied, "secure field"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    if (auto allowed = require(ShellCapability::ClipboardText); !allowed) {
        record(allowed.error().kind, allowed.error().detail);
        return allowed;
    }
    if (auto started = begin(OperationKind::ClipboardRead); !started) {
        record(started.error().kind, started.error().detail);
        return started;
    }
    ++counters_.clipboard_reads;
    const runtime::CancellationToken token = clipboard_cancel_.token();
    std::weak_ptr<ShellServiceImpl> weak = weak_from_this();
    const bool accepted = backend_->start_clipboard_read(
        token, [weak, on_done = std::move(on_done)](std::optional<ClipboardContent> content) {
            auto self = weak.lock();
            if (self == nullptr) {
                return;
            }
            self->end(OperationKind::ClipboardRead);
            if (!on_done) {
                return;
            }
            if (content.has_value() && content->text.size() > kMaxShellClipboardBytes) {
                // A back end that ignored the bound is caught here rather than truncated.
                ShellError error{ShellErrorKind::TooLarge, "clipboard read"};
                self->record(error.kind, error.detail);
                self->post(
                    [on_done = std::move(on_done), error]() { on_done(std::unexpected(error)); });
                return;
            }
            self->post([on_done = std::move(on_done), content]() {
                on_done(std::expected<std::optional<ClipboardContent>, ShellError>(content));
            });
        });
    if (!accepted) {
        end(OperationKind::ClipboardRead);
        ShellError error{ShellErrorKind::Unsupported, "clipboard read refused"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    return {};
}

std::expected<void, ShellError>
ShellServiceImpl::submit_clipboard_write(ClipboardContent content,
                                         ShellService::WriteCallback on_done) {
    if (content.text.size() > kMaxShellClipboardBytes) {
        ShellError error{ShellErrorKind::TooLarge, "clipboard write"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    if (auto allowed = require(ShellCapability::ClipboardText); !allowed) {
        record(allowed.error().kind, allowed.error().detail);
        return allowed;
    }
    if (auto started = begin(OperationKind::ClipboardWrite); !started) {
        record(started.error().kind, started.error().detail);
        return started;
    }
    ++counters_.clipboard_writes;
    const runtime::CancellationToken token = clipboard_cancel_.token();
    std::weak_ptr<ShellServiceImpl> weak = weak_from_this();
    const bool accepted = backend_->start_clipboard_write(
        std::move(content), token, [weak, on_done = std::move(on_done)](bool ok) {
            auto self = weak.lock();
            if (self == nullptr) {
                return;
            }
            self->end(OperationKind::ClipboardWrite);
            if (!on_done) {
                return;
            }
            self->post([on_done = std::move(on_done), ok]() {
                if (ok) {
                    on_done(std::expected<void, ShellError>{});
                } else {
                    on_done(std::unexpected(ShellError{ShellErrorKind::Failed, "clipboard write"}));
                }
            });
        });
    if (!accepted) {
        end(OperationKind::ClipboardWrite);
        ShellError error{ShellErrorKind::Unsupported, "clipboard write refused"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    return {};
}

std::expected<void, ShellError>
ShellServiceImpl::submit_open_url(std::string url, ShellService::WriteCallback on_done) {
    if (url.empty() || url.size() > kMaxShellUrlBytes) {
        ShellError error{url.empty() ? ShellErrorKind::Failed : ShellErrorKind::TooLarge,
                         "open url"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    if (auto allowed = require(ShellCapability::OpenUrl); !allowed) {
        record(allowed.error().kind, allowed.error().detail);
        return allowed;
    }
    ++counters_.urls_opened;
    std::weak_ptr<ShellServiceImpl> weak = weak_from_this();
    const bool accepted = backend_->start_open_url(
        std::move(url), runtime::CancellationToken{},
        [weak, on_done = std::move(on_done)](bool ok) {
            auto self = weak.lock();
            if (self == nullptr || !on_done) {
                return;
            }
            self->post([on_done = std::move(on_done), ok]() {
                if (ok) {
                    on_done(std::expected<void, ShellError>{});
                } else {
                    on_done(std::unexpected(ShellError{ShellErrorKind::Failed, "open url"}));
                }
            });
        });
    if (!accepted) {
        ShellError error{ShellErrorKind::Unsupported, "open url refused"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    return {};
}

bool ShellServiceImpl::cancel_clipboard() {
    if (!clipboard_in_flight_ || clipboard_cancelled_) {
        return false;
    }
    clipboard_cancelled_ = true;
    clipboard_cancel_.cancel();
    return true;
}

// --- dialogs -----------------------------------------------------------------------------------

std::expected<void, ShellError>
ShellServiceImpl::submit_dialog(DialogRequest request, ShellService::DialogCallback on_done) {
    ShellCapability capability = ShellCapability::FileOpen;
    switch (request.kind) {
    case DialogKind::Open:
        capability = ShellCapability::FileOpen;
        break;
    case DialogKind::Save:
        capability = ShellCapability::FileSave;
        break;
    case DialogKind::Folder:
        capability = ShellCapability::FolderPick;
        break;
    }
    if (auto allowed = require(capability); !allowed) {
        record(allowed.error().kind, allowed.error().detail);
        return allowed;
    }
    if (auto started = begin(OperationKind::Dialog); !started) {
        record(started.error().kind, started.error().detail);
        return started;
    }
    ++counters_.dialogs_opened;
    const runtime::CancellationToken token = dialog_cancel_.token();
    std::weak_ptr<ShellServiceImpl> weak = weak_from_this();
    const bool accepted = backend_->start_dialog(
        std::move(request), token, [weak, on_done = std::move(on_done)](DialogResult result) {
            auto self = weak.lock();
            if (self == nullptr) {
                return;
            }
            self->end(OperationKind::Dialog);
            if (result.cancelled) {
                ++self->counters_.dialogs_cancelled;
            }
            if (!on_done) {
                return;
            }
            // Every returned path is validated before any handle is issued (section 8). One refused
            // path fails the whole result: a caller that got three of four files would have no way
            // to know which one was dropped, and issuing the first two and then failing would leak
            // two live handles for a result the caller never receives.
            for (const std::string& path : result.paths) {
                const PathRejection rejection = shell_path_check(path);
                if (rejection != PathRejection::None) {
                    ShellError error{ShellErrorKind::InvalidPath,
                                     redacted_detail("path", path_rejection_name(rejection))};
                    self->record(error.kind, error.detail);
                    self->post([on_done = std::move(on_done), error]() {
                        on_done(std::unexpected(error));
                    });
                    return;
                }
            }
            std::vector<FileHandle> handles;
            handles.reserve(result.paths.size());
            for (std::string& path : result.paths) {
                auto handle = self->issue_handle(std::move(path));
                if (!handle) {
                    // Unreachable: the loop above validated every path with the same rule. Kept so
                    // a future divergence between the two loops cannot silently drop a file.
                    ShellError error = handle.error();
                    self->record(error.kind, error.detail);
                    self->post([on_done = std::move(on_done), error]() {
                        on_done(std::unexpected(error));
                    });
                    return;
                }
                handles.push_back(std::move(*handle));
            }
            self->post([on_done = std::move(on_done), handles = std::move(handles)]() mutable {
                on_done(std::expected<std::vector<FileHandle>, ShellError>(std::move(handles)));
            });
        });
    if (!accepted) {
        end(OperationKind::Dialog);
        ShellError error{ShellErrorKind::Unsupported, "dialog refused"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    return {};
}

bool ShellServiceImpl::cancel_dialog() {
    if (!dialog_in_flight_ || dialog_cancelled_) {
        return false;
    }
    dialog_cancelled_ = true;
    dialog_cancel_.cancel();
    return true;
}

// --- drag and drop -----------------------------------------------------------------------------

std::expected<void, ShellError> ShellServiceImpl::submit_drop(DropRequest request,
                                                              ShellService::DropCallback on_done) {
    if (auto allowed = require(ShellCapability::DropFiles); !allowed) {
        record(allowed.error().kind, allowed.error().detail);
        return allowed;
    }
    // Section 9.1's budget, enforced before the back end sees the payload.
    if (request.payload.kind == DragPayloadKind::FilePromise) {
        if (request.payload.text.size() > config_.max_drop_bytes) {
            ShellError error{ShellErrorKind::TooLarge, "drop payload"};
            record(error.kind, error.detail);
            return std::unexpected(std::move(error));
        }
        const PathRejection rejection = shell_path_check(request.payload.text);
        if (rejection != PathRejection::None) {
            ShellError error{ShellErrorKind::InvalidPath,
                             redacted_detail("drop", path_rejection_name(rejection))};
            record(error.kind, error.detail);
            return std::unexpected(std::move(error));
        }
    }
    if (request.payload.kind == DragPayloadKind::Text &&
        request.payload.text.size() > kMaxShellDragTextBytes) {
        ShellError error{ShellErrorKind::TooLarge, "drop text"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    if (auto started = begin(OperationKind::Drop); !started) {
        record(started.error().kind, started.error().detail);
        return started;
    }
    const runtime::CancellationToken token = drop_cancel_.token();
    std::weak_ptr<ShellServiceImpl> weak = weak_from_this();
    const bool accepted = backend_->start_drop(
        std::move(request), token, [weak, on_done = std::move(on_done)](DropAction action) {
            auto self = weak.lock();
            if (self == nullptr) {
                return;
            }
            self->end(OperationKind::Drop);
            if (action == DropAction::None) {
                ++self->counters_.drops_rejected;
            } else {
                ++self->counters_.drops_accepted;
            }
            if (!on_done) {
                return;
            }
            self->post([on_done = std::move(on_done), action]() {
                on_done(std::expected<DropAction, ShellError>(action));
            });
        });
    if (!accepted) {
        end(OperationKind::Drop);
        ShellError error{ShellErrorKind::Unsupported, "drop refused"};
        record(error.kind, error.detail);
        return std::unexpected(std::move(error));
    }
    return {};
}

} // namespace tessera::appfw::detail
