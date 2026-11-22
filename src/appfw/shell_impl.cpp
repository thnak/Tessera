// SPDX-License-Identifier: MIT

#include "shell_impl.hpp"
#include "tessera/appfw/shell_path.hpp"

#include <utility>

namespace tessera::appfw::detail {

std::string redacted_detail(std::string_view operation, std::string_view reason) {
    std::string detail;
    detail.reserve(operation.size() + reason.size() + 3);
    detail.append(operation);
    detail.append(": ");
    detail.append(reason);
    return detail;
}

ShellServiceImpl::ShellServiceImpl(ShellConfig config, ShellBackend& backend,
                                   runtime::CompletionQueue& completions)
    : config_(config),
      backend_(&backend),
      completions_(&completions) {
    capabilities_ = backend.capabilities().capabilities;
    counters_.capacity = config_.diagnostic_capacity;
}

ShellServiceImpl::~ShellServiceImpl() = default;

void ShellServiceImpl::refresh_capabilities() {
    capabilities_ = backend_->capabilities().capabilities;
}

std::expected<void, ShellError> ShellServiceImpl::require(ShellCapability capability) const {
    if (!capabilities_.supports(capability)) {
        return std::unexpected(
            ShellError{ShellErrorKind::Unsupported,
                       redacted_detail("capability", shell_capability_name(capability))});
    }
    if (!capabilities_.permits(capability)) {
        return std::unexpected(
            ShellError{ShellErrorKind::Denied,
                       redacted_detail("permission", shell_capability_name(capability))});
    }
    return {};
}

std::expected<void, ShellError> ShellServiceImpl::begin(OperationKind kind) {
    bool* flag = nullptr;
    switch (kind) {
    case OperationKind::ClipboardRead:
    case OperationKind::ClipboardWrite:
        flag = &clipboard_in_flight_;
        break;
    case OperationKind::Dialog:
        flag = &dialog_in_flight_;
        break;
    case OperationKind::Drop:
        flag = &drop_in_flight_;
        break;
    case OperationKind::None:
        return {};
    }
    if (*flag) {
        return std::unexpected(ShellError{ShellErrorKind::Busy, "operation already in flight"});
    }
    *flag = true;
    ++in_flight_;
    return {};
}

void ShellServiceImpl::end(OperationKind kind) {
    bool* flag = nullptr;
    switch (kind) {
    case OperationKind::ClipboardRead:
    case OperationKind::ClipboardWrite:
        flag = &clipboard_in_flight_;
        // A resolved operation clears the cancel latch and starts a fresh source, so the next
        // operation is cancellable again and a stale cancellation cannot leak into it.
        clipboard_cancelled_ = false;
        clipboard_cancel_ = runtime::CancellationSource{};
        break;
    case OperationKind::Dialog:
        flag = &dialog_in_flight_;
        dialog_cancelled_ = false;
        dialog_cancel_ = runtime::CancellationSource{};
        break;
    case OperationKind::Drop:
        flag = &drop_in_flight_;
        drop_cancelled_ = false;
        drop_cancel_ = runtime::CancellationSource{};
        break;
    case OperationKind::None:
        return;
    }
    if (*flag) {
        *flag = false;
        if (in_flight_ > 0) {
            --in_flight_;
        }
    }
}

void ShellServiceImpl::resolve_cancelled(OperationKind kind) {
    switch (kind) {
    case OperationKind::ClipboardRead:
    case OperationKind::ClipboardWrite:
        clipboard_cancel_.cancel();
        break;
    case OperationKind::Dialog:
        dialog_cancel_.cancel();
        break;
    case OperationKind::Drop:
        drop_cancel_.cancel();
        break;
    case OperationKind::None:
        break;
    }
}

bool ShellServiceImpl::cancel_drop() {
    if (!drop_in_flight_ || drop_cancelled_) {
        return false;
    }
    drop_cancelled_ = true;
    drop_cancel_.cancel();
    return true;
}

void ShellServiceImpl::post(std::function<void()> callback) {
    if (!callback) {
        return;
    }
    completions_->post(runtime::CompletionPriority::Visible, std::move(callback));
}

void ShellServiceImpl::record(ShellErrorKind kind, std::string detail) {
    switch (kind) {
    case ShellErrorKind::Unsupported:
        ++counters_.refused_unsupported;
        break;
    case ShellErrorKind::Denied:
        ++counters_.refused_denied;
        break;
    case ShellErrorKind::Busy:
        ++counters_.refused_busy;
        break;
    case ShellErrorKind::TooLarge:
        ++counters_.refused_too_large;
        break;
    case ShellErrorKind::InvalidPath:
        ++counters_.refused_invalid_path;
        break;
    case ShellErrorKind::Cancelled:
    case ShellErrorKind::Failed:
        break;
    }
    if (config_.diagnostic_capacity == 0) {
        return;
    }
    if (ring_.size() >= config_.diagnostic_capacity) {
        ring_.pop_front();
        ++dropped_;
    }
    ring_.push_back(ShellDiagnostic{kind, std::move(detail)});
    ++recorded_;
}

std::expected<FileHandle, ShellError> ShellServiceImpl::issue_handle(std::string path) {
    const PathRejection rejection = shell_path_check(path);
    if (rejection != PathRejection::None) {
        return std::unexpected(ShellError{ShellErrorKind::InvalidPath,
                                          redacted_detail("path", path_rejection_name(rejection))});
    }
    FileHandle handle;
    handle.path = std::move(path);
    handle.token = next_token_++;
    handles_.push_back(HandleEntry{handle.token, handle.path});
    return handle;
}

bool ShellServiceImpl::validate(const FileHandle& handle) const {
    if (!handle.valid()) {
        return false;
    }
    for (const HandleEntry& entry : handles_) {
        if (entry.token == handle.token) {
            return entry.path == handle.path;
        }
    }
    return false;
}

bool ShellServiceImpl::revoke(const FileHandle& handle) {
    for (auto it = handles_.begin(); it != handles_.end(); ++it) {
        if (it->token == handle.token) {
            handles_.erase(it);
            return true;
        }
    }
    return false;
}

ShellDiagnostics ShellServiceImpl::diagnostics() const {
    ShellDiagnostics snapshot = counters_;
    snapshot.recorded = recorded_;
    snapshot.dropped = dropped_;
    snapshot.capacity = config_.diagnostic_capacity;
    snapshot.entries.assign(ring_.begin(), ring_.end());
    return snapshot;
}

std::string_view ShellServiceImpl::backend_name() const noexcept {
    return backend_->name();
}

} // namespace tessera::appfw::detail
