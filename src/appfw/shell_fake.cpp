// SPDX-License-Identifier: MIT

#include "tessera/appfw/shell_fake.hpp"

#include <utility>

namespace tessera::appfw {

ShellCapabilities desktop_shell_capabilities() noexcept {
    ShellCapabilities capabilities;
    capabilities.supported = shell_capability_bit(ShellCapability::ClipboardText) |
                             shell_capability_bit(ShellCapability::DragInternal) |
                             shell_capability_bit(ShellCapability::DragExternal) |
                             shell_capability_bit(ShellCapability::DropFiles) |
                             shell_capability_bit(ShellCapability::ContextMenu) |
                             shell_capability_bit(ShellCapability::FileOpen) |
                             shell_capability_bit(ShellCapability::FileSave) |
                             shell_capability_bit(ShellCapability::FolderPick) |
                             shell_capability_bit(ShellCapability::OpenUrl);
    capabilities.permissioned = capabilities.supported;
    return capabilities;
}

FakeShell::FakeShell(runtime::CompletionQueue& completions, runtime::Clock& clock,
                     ShellCapabilities capabilities)
    : completions_(&completions),
      clock_(&clock),
      capabilities_(capabilities) {}

void FakeShell::push_clipboard_read(std::optional<ClipboardContent> content,
                                    std::uint32_t delay_ms) {
    read_script_.push_back(FakeClipboardScript{std::move(content), delay_ms});
}

void FakeShell::push_clipboard_write(bool ok, std::uint32_t delay_ms) {
    write_script_.emplace_back(ok, delay_ms);
}

void FakeShell::push_dialog(DialogResult result, std::uint32_t delay_ms) {
    dialog_script_.push_back(FakeDialogScript{std::move(result), delay_ms});
}

void FakeShell::push_dialog_paths(std::vector<std::string> paths, std::uint32_t delay_ms) {
    DialogResult result;
    result.paths = std::move(paths);
    push_dialog(std::move(result), delay_ms);
}

void FakeShell::push_dialog_cancel(std::uint32_t delay_ms) {
    DialogResult result;
    result.cancelled = true;
    push_dialog(std::move(result), delay_ms);
}

void FakeShell::push_drop(DropAction action, std::uint32_t delay_ms) {
    drop_script_.push_back(FakeDropScript{action, delay_ms});
}

void FakeShell::set_capabilities(ShellCapabilities capabilities) noexcept {
    capabilities_ = capabilities;
}

ShellBackendCapabilities FakeShell::capabilities() const {
    return ShellBackendCapabilities{capabilities_};
}

void FakeShell::set_available(bool available) noexcept {
    available_ = available;
}

bool FakeShell::token_cancelled(const runtime::CancellationToken& token) const {
    return token.cancelled();
}

std::size_t FakeShell::cancelled_operations() const noexcept {
    return cancelled_operations_;
}

std::string_view FakeShell::name() const noexcept {
    return "fake";
}

bool FakeShell::start_clipboard_read(runtime::CancellationToken token,
                                     ClipboardReadCompletion on_done) {
    if (!available_) {
        return false;
    }
    received_.push_back(
        FakeShellCall{FakeShellCall::Kind::ClipboardRead, DialogKind::Open, {}, {}});
    if (token_cancelled(token)) {
        ++cancelled_operations_;
        return true; // accepted, then dropped: contract 2 allows reporting nothing
    }
    Pending pending;
    pending.kind = FakeShellCall::Kind::ClipboardRead;
    pending.token = token;
    std::uint32_t delay_ms = 0;
    if (read_cursor_ < read_script_.size()) {
        pending.read_content = std::move(read_script_[read_cursor_].content);
        delay_ms = read_script_[read_cursor_].delay_ms;
        ++read_cursor_;
    }
    pending.due_seconds = clock_->now_seconds() + static_cast<double>(delay_ms) / 1000.0;
    pending.on_read = std::move(on_done);
    pending_.push_back(std::move(pending));
    return true;
}

bool FakeShell::start_clipboard_write(ClipboardContent content, runtime::CancellationToken token,
                                      ClipboardWriteCompletion on_done) {
    if (!available_) {
        return false;
    }
    received_.push_back(
        FakeShellCall{FakeShellCall::Kind::ClipboardWrite, DialogKind::Open, content.text, {}});
    if (token_cancelled(token)) {
        ++cancelled_operations_;
        return true;
    }
    Pending pending;
    pending.kind = FakeShellCall::Kind::ClipboardWrite;
    pending.token = token;
    std::uint32_t delay_ms = 0;
    if (write_cursor_ < write_script_.size()) {
        pending.write_ok = write_script_[write_cursor_].first;
        delay_ms = write_script_[write_cursor_].second;
        ++write_cursor_;
    }
    pending.due_seconds = clock_->now_seconds() + static_cast<double>(delay_ms) / 1000.0;
    pending.on_write = std::move(on_done);
    pending_.push_back(std::move(pending));
    return true;
}

bool FakeShell::start_open_url(std::string url, runtime::CancellationToken token,
                               OpenUrlCompletion on_done) {
    if (!available_) {
        return false;
    }
    received_.push_back(FakeShellCall{FakeShellCall::Kind::OpenUrl, DialogKind::Open, url, {}});
    if (token_cancelled(token)) {
        ++cancelled_operations_;
        return true;
    }
    // A URL hand-off has no script or delay: it completes on the next queue drain, like the
    // platform call returning.
    if (on_done) {
        completions_->post(runtime::CompletionPriority::Visible,
                           [on_done = std::move(on_done), ok = open_url_ok_]() { on_done(ok); });
    }
    return true;
}

bool FakeShell::start_dialog(DialogRequest request, runtime::CancellationToken token,
                             DialogCompletion on_done) {
    if (!available_) {
        return false;
    }
    FakeShellCall call;
    call.kind = FakeShellCall::Kind::Dialog;
    call.dialog_kind = request.kind;
    switch (request.kind) {
    case DialogKind::Open:
        call.default_directory = request.open.default_directory;
        break;
    case DialogKind::Save:
        call.default_directory = request.save.default_directory;
        break;
    case DialogKind::Folder:
        call.default_directory = request.folder.default_directory;
        break;
    }
    received_.push_back(std::move(call));
    if (token_cancelled(token)) {
        ++cancelled_operations_;
        return true;
    }
    Pending pending;
    pending.kind = FakeShellCall::Kind::Dialog;
    pending.token = token;
    std::uint32_t delay_ms = 0;
    if (dialog_cursor_ < dialog_script_.size()) {
        pending.dialog = std::move(dialog_script_[dialog_cursor_].result);
        delay_ms = dialog_script_[dialog_cursor_].delay_ms;
        ++dialog_cursor_;
    } else {
        pending.dialog.cancelled = true;
    }
    pending.due_seconds = clock_->now_seconds() + static_cast<double>(delay_ms) / 1000.0;
    pending.on_dialog = std::move(on_done);
    pending_.push_back(std::move(pending));
    return true;
}

bool FakeShell::start_drop(DropRequest request, runtime::CancellationToken token,
                           DropCompletion on_done) {
    if (!available_) {
        return false;
    }
    received_.push_back(
        FakeShellCall{FakeShellCall::Kind::Drop, DialogKind::Open, request.payload.text, {}});
    if (token_cancelled(token)) {
        ++cancelled_operations_;
        return true;
    }
    Pending pending;
    pending.kind = FakeShellCall::Kind::Drop;
    pending.token = token;
    std::uint32_t delay_ms = 0;
    if (drop_cursor_ < drop_script_.size()) {
        pending.drop = drop_script_[drop_cursor_].action;
        delay_ms = drop_script_[drop_cursor_].delay_ms;
        ++drop_cursor_;
    }
    pending.due_seconds = clock_->now_seconds() + static_cast<double>(delay_ms) / 1000.0;
    pending.on_drop = std::move(on_done);
    pending_.push_back(std::move(pending));
    return true;
}

std::size_t FakeShell::deliver_due() {
    const double now = clock_->now_seconds();
    std::size_t delivered = 0;
    std::vector<Pending> remaining;
    remaining.reserve(pending_.size());
    for (Pending& entry : pending_) {
        if (entry.due_seconds > now) {
            remaining.push_back(std::move(entry));
            continue;
        }
        if (token_cancelled(entry.token)) {
            ++cancelled_operations_;
            continue;
        }
        ++delivered;
        switch (entry.kind) {
        case FakeShellCall::Kind::ClipboardRead: {
            auto callback = std::move(entry.on_read);
            if (callback) {
                std::optional<ClipboardContent> content = std::move(entry.read_content);
                completions_->post(runtime::CompletionPriority::Visible,
                                   [callback = std::move(callback), content]() mutable {
                                       callback(std::move(content));
                                   });
            }
            break;
        }
        case FakeShellCall::Kind::ClipboardWrite: {
            auto callback = std::move(entry.on_write);
            if (callback) {
                const bool ok = entry.write_ok;
                completions_->post(runtime::CompletionPriority::Visible,
                                   [callback = std::move(callback), ok]() { callback(ok); });
            }
            break;
        }
        case FakeShellCall::Kind::OpenUrl:
            break;
        case FakeShellCall::Kind::Dialog: {
            auto callback = std::move(entry.on_dialog);
            if (callback) {
                DialogResult result = std::move(entry.dialog);
                completions_->post(
                    runtime::CompletionPriority::Visible,
                    [callback = std::move(callback), result = std::move(result)]() mutable {
                        callback(std::move(result));
                    });
            }
            break;
        }
        case FakeShellCall::Kind::Drop: {
            auto callback = std::move(entry.on_drop);
            if (callback) {
                const DropAction action = entry.drop;
                completions_->post(
                    runtime::CompletionPriority::Visible,
                    [callback = std::move(callback), action]() { callback(action); });
            }
            break;
        }
        }
    }
    pending_ = std::move(remaining);
    return delivered;
}

void FakeShell::clear() {
    read_script_.clear();
    write_script_.clear();
    dialog_script_.clear();
    drop_script_.clear();
    read_cursor_ = 0;
    write_cursor_ = 0;
    dialog_cursor_ = 0;
    drop_cursor_ = 0;
    pending_.clear();
}

} // namespace tessera::appfw
