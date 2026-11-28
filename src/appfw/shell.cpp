// SPDX-License-Identifier: MIT

#include "shell_impl.hpp"
#include "tessera/appfw/shell.hpp"

#include <coroutine>
#include <memory>
#include <optional>
#include <utility>

namespace tessera::appfw {
namespace {

// Shared between a coroutine frame and the service's completion callback. The callback can arrive
// before the coroutine reaches `await_suspend` (a fake back end may resolve on the next queue
// drain, and a caller may start the task after that), so "which came first" is carried in the state
// instead of being assumed. This is the same shape `NetClient::send` uses, for the same reason.
template<typename T> struct ShellAwaitState {
    std::optional<T> result;
    std::coroutine_handle<> handle;
    bool resume_posted = false;
    // Set when the coroutine frame is destroyed before its operation resolved - a caller that drops
    // the task it was awaiting. A posted or later resume then does nothing, instead of resuming a
    // destroyed frame.
    bool abandoned = false;
};

// The awaiter a coroutine body suspends on. `T` is the `expected<...>` the operation delivers.
template<typename T> class ShellAwaiter {
public:
    ShellAwaiter(runtime::CompletionQueue& queue, std::shared_ptr<ShellAwaitState<T>> state)
        : queue_(&queue),
          state_(std::move(state)) {}

    // A destroyed frame must not be resumed by a completion that is already in the queue.
    ~ShellAwaiter() {
        if (state_ != nullptr) {
            state_->abandoned = true;
        }
    }

    ShellAwaiter(const ShellAwaiter&) = delete;
    ShellAwaiter& operator=(const ShellAwaiter&) = delete;

    // False even when the result already arrived: `await_resume` has not run yet, and posting the
    // resume keeps the coroutine off the resolving stack.
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) const {
        state_->handle = handle;
        if (!state_->result.has_value() || state_->resume_posted) {
            return;
        }
        // The result beat the suspension: post the resume rather than returning false, so the
        // coroutine still continues from the queue instead of from this stack.
        state_->resume_posted = true;
        queue_->post(runtime::CompletionPriority::Visible,
                     [state = state_]() { resume_if_live(*state); });
    }

    // The one place a coroutine frame is resumed, so the `abandoned` guard cannot be forgotten.
    static void resume_if_live(ShellAwaitState<T>& state) {
        if (state.abandoned || state.handle == nullptr) {
            return;
        }
        state.handle.resume();
    }

    T await_resume() {
        if (!state_->result.has_value()) {
            return std::unexpected(
                ShellError{ShellErrorKind::Failed, "the operation never resolved"});
        }
        return std::move(*state_->result);
    }

private:
    runtime::CompletionQueue* queue_ = nullptr;
    std::shared_ptr<ShellAwaitState<T>> state_;
};

// The completion a service callback runs: stores the result and resumes through the queue, exactly
// once. A completion that arrives after the frame was abandoned stores nothing and resumes nothing.
template<typename T>
void deliver(std::shared_ptr<ShellAwaitState<T>> state, runtime::CompletionQueue& queue, T result) {
    if (state->result.has_value() || state->abandoned) {
        return;
    }
    state->result.emplace(std::move(result));
    if (state->handle == nullptr || state->resume_posted) {
        return;
    }
    state->resume_posted = true;
    queue.post(runtime::CompletionPriority::Visible,
               [state]() { ShellAwaiter<T>::resume_if_live(*state); });
}

} // namespace

ShellService::ShellService(ShellConfig config, ShellBackend& backend,
                           runtime::CompletionQueue& completions)
    : impl_(std::make_shared<detail::ShellServiceImpl>(config, backend, completions)) {}

ShellService::~ShellService() = default;

ShellCapabilities ShellService::capabilities() const {
    return impl_->capabilities();
}

void ShellService::refresh_capabilities() {
    impl_->refresh_capabilities();
}

std::expected<void, ShellError> ShellService::require(ShellCapability capability) const {
    return impl_->require(capability);
}

std::expected<void, ShellError> ShellService::submit_clipboard_read(bool secure,
                                                                    ClipboardCallback on_done) {
    return impl_->submit_clipboard_read(secure, std::move(on_done));
}

std::expected<void, ShellError> ShellService::submit_clipboard_write(ClipboardContent content,
                                                                     WriteCallback on_done) {
    return impl_->submit_clipboard_write(std::move(content), std::move(on_done));
}

std::expected<void, ShellError> ShellService::submit_open_url(std::string url,
                                                              WriteCallback on_done) {
    return impl_->submit_open_url(std::move(url), std::move(on_done));
}

std::expected<void, ShellError> ShellService::submit_open_dialog(OpenDialogSpec spec,
                                                                 DialogCallback on_done) {
    DialogRequest request;
    request.kind = DialogKind::Open;
    request.open = std::move(spec);
    return impl_->submit_dialog(std::move(request), std::move(on_done));
}

std::expected<void, ShellError> ShellService::submit_save_dialog(SaveDialogSpec spec,
                                                                 DialogCallback on_done) {
    DialogRequest request;
    request.kind = DialogKind::Save;
    request.save = std::move(spec);
    return impl_->submit_dialog(std::move(request), std::move(on_done));
}

std::expected<void, ShellError> ShellService::submit_folder_dialog(FolderDialogSpec spec,
                                                                   DialogCallback on_done) {
    DialogRequest request;
    request.kind = DialogKind::Folder;
    request.folder = std::move(spec);
    return impl_->submit_dialog(std::move(request), std::move(on_done));
}

bool ShellService::cancel_dialog() {
    return impl_->cancel_dialog();
}

bool ShellService::cancel_clipboard() {
    return impl_->cancel_clipboard();
}

std::expected<void, ShellError> ShellService::submit_drop(DropRequest request,
                                                          DropCallback on_done) {
    return impl_->submit_drop(std::move(request), std::move(on_done));
}

bool ShellService::validate(const FileHandle& handle) const {
    return impl_->validate(handle);
}

bool ShellService::revoke(const FileHandle& handle) {
    return impl_->revoke(handle);
}

std::size_t ShellService::live_handles() const noexcept {
    return impl_->live_handles();
}

const ShellConfig& ShellService::config() const noexcept {
    return impl_->config();
}

ShellDiagnostics ShellService::diagnostics() const {
    return impl_->diagnostics();
}

std::string_view ShellService::backend_name() const noexcept {
    return impl_->backend_name();
}

std::size_t ShellService::in_flight() const noexcept {
    return impl_->in_flight();
}

// --- the coroutine forms -----------------------------------------------------------------------
//
// Each body submits the callback form and parks until the completion arrives. The task's own token
// is empty (see the header), so `Task` never throws `TaskCancelled` here and every outcome -
// including cancellation - is a value in the `expected`.

runtime::Task<std::expected<std::optional<ClipboardContent>, ShellError>>
ShellService::read_clipboard(bool secure) {
    runtime::TaskContext context;
    context.queue = &impl_->completions();
    context.priority = runtime::CompletionPriority::Visible;
    return read_body(context, *this, secure);
}

runtime::Task<std::expected<std::optional<ClipboardContent>, ShellError>>
ShellService::read_body(runtime::TaskContext, ShellService& self, bool secure) {
    using Result = std::expected<std::optional<ClipboardContent>, ShellError>;
    auto state = std::make_shared<ShellAwaitState<Result>>();
    runtime::CompletionQueue& queue = self.impl_->completions();
    auto submitted = self.submit_clipboard_read(
        secure, [state, &queue](Result result) { deliver(state, queue, std::move(result)); });
    if (!submitted) {
        co_return std::unexpected(submitted.error());
    }
    co_return co_await ShellAwaiter<Result>{queue, std::move(state)};
}

runtime::Task<std::expected<void, ShellError>>
ShellService::write_clipboard(ClipboardContent content) {
    runtime::TaskContext context;
    context.queue = &impl_->completions();
    context.priority = runtime::CompletionPriority::Visible;
    return write_body(context, *this, std::move(content));
}

runtime::Task<std::expected<void, ShellError>>
ShellService::write_body(runtime::TaskContext, ShellService& self, ClipboardContent content) {
    using Result = std::expected<void, ShellError>;
    auto state = std::make_shared<ShellAwaitState<Result>>();
    runtime::CompletionQueue& queue = self.impl_->completions();
    auto submitted =
        self.submit_clipboard_write(std::move(content), [state, &queue](Result result) {
            deliver(state, queue, std::move(result));
        });
    if (!submitted) {
        co_return std::unexpected(submitted.error());
    }
    co_return co_await ShellAwaiter<Result>{queue, std::move(state)};
}

runtime::Task<std::expected<std::vector<FileHandle>, ShellError>>
ShellService::open_file(OpenDialogSpec spec) {
    DialogRequest request;
    request.kind = DialogKind::Open;
    request.open = std::move(spec);
    runtime::TaskContext context;
    context.queue = &impl_->completions();
    context.priority = runtime::CompletionPriority::Visible;
    return dialog_body(context, *this, std::move(request));
}

runtime::Task<std::expected<std::vector<FileHandle>, ShellError>>
ShellService::save_file(SaveDialogSpec spec) {
    DialogRequest request;
    request.kind = DialogKind::Save;
    request.save = std::move(spec);
    runtime::TaskContext context;
    context.queue = &impl_->completions();
    context.priority = runtime::CompletionPriority::Visible;
    return dialog_body(context, *this, std::move(request));
}

runtime::Task<std::expected<std::vector<FileHandle>, ShellError>>
ShellService::pick_folder(FolderDialogSpec spec) {
    DialogRequest request;
    request.kind = DialogKind::Folder;
    request.folder = std::move(spec);
    runtime::TaskContext context;
    context.queue = &impl_->completions();
    context.priority = runtime::CompletionPriority::Visible;
    return dialog_body(context, *this, std::move(request));
}

runtime::Task<std::expected<std::vector<FileHandle>, ShellError>>
ShellService::dialog_body(runtime::TaskContext, ShellService& self, DialogRequest request) {
    using Result = std::expected<std::vector<FileHandle>, ShellError>;
    auto state = std::make_shared<ShellAwaitState<Result>>();
    runtime::CompletionQueue& queue = self.impl_->completions();
    auto submitted = self.impl_->submit_dialog(std::move(request), [state, &queue](Result result) {
        deliver(state, queue, std::move(result));
    });
    if (!submitted) {
        co_return std::unexpected(submitted.error());
    }
    co_return co_await ShellAwaiter<Result>{queue, std::move(state)};
}

} // namespace tessera::appfw
