// SPDX-License-Identifier: MIT
//
// `WinHttpTransport` (issue #835). See net_winhttp.hpp for the contract; this file is the callback
// state machine.
//
// One request is one `Op`, driven by WinHTTP's status callback:
//
//   SENDREQUEST_COMPLETE -> ReceiveResponse -> HEADERS_AVAILABLE -> QueryDataAvailable
//     -> DATA_AVAILABLE -> ReadData -> READ_COMPLETE -> QueryDataAvailable ... -> 0 bytes: done
//
// **Lifetime.** The op is owned by nobody but the OS handle: it is deleted in
// `HANDLE_CLOSING`, the last callback WinHTTP delivers for the request handle, and the connection
// handle is closed there too. `HANDLE_CLOSING` is also where a result that nobody has reported yet
// (a cancel that closed the handle before any error callback) is reported, so exactly one
// completion is posted per accepted `start`.
//
// **Cancellation.** The token's callback runs on whichever thread cancels, so it never calls into
// WinHTTP itself: it hands the request's close to a closer thread this transport owns. That keeps a
// registration that is torn down on a WinHTTP thread from ever waiting on a callback that is itself
// waiting for WinHTTP.

#include "tessera/appfw/net_winhttp.hpp"
#include "tessera/appfw/net_winhttp_map.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <windows.h>
#include <winhttp.h>

namespace tessera::appfw {
namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// The request handle, shared between the op and the token's callback so a late cancel finds either
// the handle or nothing, never a dangling one. Closing takes the handle out under the lock and
// closes it outside, so the close can never run while the lock is held.
struct CloseHook {
    std::mutex mutex;
    HINTERNET request = nullptr;
    std::atomic<bool> cancelled{false};

    HINTERNET take() {
        std::lock_guard lock(mutex);
        return std::exchange(request, nullptr);
    }
};

} // namespace

struct WinHttpTransport::Impl {
    explicit Impl(runtime::CompletionQueue& queue)
        : completions(&queue) {}

    runtime::CompletionQueue* completions;
    HINTERNET session = nullptr;

    // Every op still holding a handle, so the destructor can cancel and wait for them.
    std::mutex ops_mutex;
    std::condition_variable ops_idle;
    std::unordered_set<void*> ops;
    std::vector<std::shared_ptr<CloseHook>> hooks;

    // The closer: a token callback enqueues, this thread closes.
    std::mutex closer_mutex;
    std::condition_variable closer_wake;
    std::deque<std::shared_ptr<CloseHook>> closer_queue;
    bool closer_stop = false;
    std::thread closer;

    void run_closer() {
        for (;;) {
            std::shared_ptr<CloseHook> hook;
            {
                std::unique_lock lock(closer_mutex);
                closer_wake.wait(lock, [this] { return closer_stop || !closer_queue.empty(); });
                if (closer_queue.empty()) {
                    return;
                }
                hook = std::move(closer_queue.front());
                closer_queue.pop_front();
            }
            if (const HINTERNET handle = hook->take()) {
                (void)WinHttpCloseHandle(handle);
            }
        }
    }

    void request_close(const std::shared_ptr<CloseHook>& hook) {
        {
            std::lock_guard lock(closer_mutex);
            closer_queue.push_back(hook);
        }
        closer_wake.notify_one();
    }
};

namespace {

struct Op {
    WinHttpTransport::Completion on_done;
    runtime::CompletionQueue* completions = nullptr;
    NetRequest request;
    std::wstring wide_headers;
    std::string body_in;
    HINTERNET connect = nullptr;
    std::shared_ptr<CloseHook> hook;
    runtime::CancellationRegistration registration;
    NetResponse response;
    std::string body;
    std::vector<char> chunk;
    bool finished = false;
    void* impl_key = nullptr;
};

void post_result(runtime::CompletionQueue& queue, WinHttpTransport::Completion done,
                 NetResult result) {
    // Posted, never called inline: the seam's contract 1.
    (void)queue.post(runtime::CompletionPriority::Visible,
                     [done = std::move(done), result = std::move(result)]() mutable {
                         done(std::move(result));
                     });
}

NetResult failure(NetErrorKind kind, std::string message, std::uint32_t code = 0) {
    return std::unexpected(NetError{kind, std::move(message), static_cast<std::int64_t>(code)});
}

// Reports once and drops the handle, which releases the connection.
void finish(Op& op, NetResult result) {
    if (op.finished) {
        return;
    }
    op.finished = true;
    post_result(*op.completions, std::move(op.on_done), std::move(result));
    if (const HINTERNET handle = op.hook->take()) {
        (void)WinHttpCloseHandle(handle);
    }
}

void fail(Op& op, std::uint32_t code) {
    const NetErrorKind kind =
        op.hook->cancelled.load() ? NetErrorKind::Cancelled : error_kind_for_winhttp_status(code);
    finish(op, failure(kind, "WinHTTP error " + std::to_string(code), code));
}

void read_headers(Op& op, HINTERNET handle) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        op.response.status = static_cast<int>(status);
    }
    DWORD raw_bytes = 0;
    (void)WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                              WINHTTP_NO_OUTPUT_BUFFER, &raw_bytes, WINHTTP_NO_HEADER_INDEX);
    if (raw_bytes >= sizeof(wchar_t)) {
        std::wstring raw(raw_bytes / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &raw_bytes,
                                WINHTTP_NO_HEADER_INDEX)) {
            const std::string text = narrow(raw.c_str());
            std::size_t position = text.find('\n'); // skip the status line
            while (position != std::string::npos && position + 1 < text.size()) {
                const std::size_t begin = position + 1;
                const std::size_t end = text.find('\n', begin);
                const std::string_view line = std::string_view(text).substr(
                    begin, end == std::string::npos ? end : end - begin);
                const std::size_t colon = line.find(':');
                if (colon != std::string_view::npos && colon > 0) {
                    op.response.headers.emplace_back(std::string(trim(line.substr(0, colon))),
                                                     std::string(trim(line.substr(colon + 1))));
                }
                position = end;
            }
        }
    }
    op.response.final_url = op.request.url;
    op.response.protocol = NetProtocol::H1;
#if defined(WINHTTP_OPTION_HTTP_PROTOCOL_USED) && defined(WINHTTP_PROTOCOL_FLAG_HTTP2)
    DWORD used = 0;
    DWORD used_size = sizeof(used);
    if (WinHttpQueryOption(handle, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &used, &used_size)) {
        if ((used & WINHTTP_PROTOCOL_FLAG_HTTP2) != 0) {
            op.response.protocol = NetProtocol::H2;
        }
#ifdef WINHTTP_PROTOCOL_FLAG_HTTP3
        if ((used & WINHTTP_PROTOCOL_FLAG_HTTP3) != 0) {
            op.response.protocol = NetProtocol::H3;
        }
#endif
    }
#endif
}

void on_status(HINTERNET handle, DWORD_PTR context, DWORD status, LPVOID info, DWORD info_length) {
    auto* const op = reinterpret_cast<Op*>(context);
    if (op == nullptr) {
        return;
    }
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        if (!WinHttpReceiveResponse(handle, nullptr)) {
            fail(*op, GetLastError());
        }
        break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        read_headers(*op, handle);
        if (!WinHttpQueryDataAvailable(handle, nullptr)) {
            fail(*op, GetLastError());
        }
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
        const DWORD available = info != nullptr ? *static_cast<DWORD*>(info) : 0;
        if (available == 0) {
            op->response.body = std::move(op->body);
            finish(*op, NetResult{std::move(op->response)});
            break;
        }
        // Section 3 item 5: stop reading rather than buffer past the cap.
        if (op->request.max_response_bytes != 0 &&
            op->body.size() + available > op->request.max_response_bytes) {
            finish(*op,
                   failure(NetErrorKind::TooLarge, "the response body exceeds max_response_bytes"));
            break;
        }
        op->chunk.resize(available);
        if (!WinHttpReadData(handle, op->chunk.data(), available, nullptr)) {
            fail(*op, GetLastError());
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        op->body.append(op->chunk.data(), info_length);
        if (!WinHttpQueryDataAvailable(handle, nullptr)) {
            fail(*op, GetLastError());
        }
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const auto* const result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
        fail(*op, result != nullptr ? static_cast<std::uint32_t>(result->dwError) : 0u);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: {
        // The last callback for this handle: report anything still unreported (a cancel that closed
        // the handle first), then release the connection and the op.
        if (!op->finished) {
            op->finished = true;
            post_result(*op->completions, std::move(op->on_done),
                        failure(NetErrorKind::Cancelled, "the request was cancelled"));
        }
        op->registration.reset();
        if (op->connect != nullptr) {
            (void)WinHttpCloseHandle(op->connect);
        }
        auto* const impl = static_cast<WinHttpTransport::Impl*>(op->impl_key);
        {
            std::lock_guard lock(impl->ops_mutex);
            impl->ops.erase(op);
            std::erase_if(impl->hooks, [&](const auto& hook) { return hook == op->hook; });
        }
        impl->ops_idle.notify_all();
        delete op;
        break;
    }
    default:
        break;
    }
}

} // namespace

WinHttpTransport::WinHttpTransport(runtime::CompletionQueue& completions)
    : impl_(std::make_unique<Impl>(completions)) {
    // An empty agent: the module sets `User-Agent` itself, and the transport adds none.
    impl_->session = WinHttpOpen(L"", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (impl_->session == nullptr) {
        return;
    }
#if defined(WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL) && defined(WINHTTP_PROTOCOL_FLAG_HTTP2)
    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
    (void)WinHttpSetOption(impl_->session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols,
                           sizeof(protocols));
#endif
    impl_->closer = std::thread([impl = impl_.get()] { impl->run_closer(); });
}

WinHttpTransport::~WinHttpTransport() {
    if (impl_ == nullptr) {
        return;
    }
    // Cancel what is in flight and wait for the OS to hand every handle back. Closing happens
    // outside the lock: a close can deliver HANDLE_CLOSING, which takes it.
    std::vector<std::shared_ptr<CloseHook>> pending;
    {
        std::lock_guard lock(impl_->ops_mutex);
        pending = impl_->hooks;
    }
    for (const auto& hook : pending) {
        hook->cancelled.store(true);
        if (const HINTERNET handle = hook->take()) {
            (void)WinHttpCloseHandle(handle);
        }
    }
    {
        std::unique_lock lock(impl_->ops_mutex);
        impl_->ops_idle.wait(lock, [this] { return impl_->ops.empty(); });
    }
    {
        std::lock_guard lock(impl_->closer_mutex);
        impl_->closer_stop = true;
    }
    impl_->closer_wake.notify_all();
    if (impl_->closer.joinable()) {
        impl_->closer.join();
    }
    if (impl_->session != nullptr) {
        (void)WinHttpCloseHandle(impl_->session);
    }
}

bool WinHttpTransport::available() const noexcept {
    return impl_ != nullptr && impl_->session != nullptr;
}

std::string_view WinHttpTransport::name() const noexcept {
    return "winhttp";
}

bool WinHttpTransport::start(const NetRequest& request, runtime::CancellationToken token,
                             Completion on_done) {
    if (!available()) {
        return false;
    }
    auto report = [&](NetResult result) {
        post_result(*impl_->completions, std::move(on_done), std::move(result));
        return true;
    };
    if (token.cancelled()) {
        return report(failure(NetErrorKind::Cancelled, "the request was cancelled"));
    }
    WinHttpUrlParts parts;
    std::string why;
    if (!split_winhttp_url(request.url, parts, why)) {
        return report(failure(NetErrorKind::BadUrl, why));
    }

    auto op = std::make_unique<Op>();
    op->completions = impl_->completions;
    op->request = request;
    op->body_in = request.has_body ? request.body : std::string();
    op->impl_key = impl_.get();
    op->hook = std::make_shared<CloseHook>();
    for (const auto& [name, value] : request.headers) {
        op->wide_headers += widen(name) + L": " + widen(value) + L"\r\n";
    }

    op->connect = WinHttpConnect(impl_->session, widen(parts.host).c_str(), parts.port, 0);
    if (op->connect == nullptr) {
        const DWORD code = GetLastError();
        return report(failure(error_kind_for_winhttp_status(code), "WinHttpConnect failed", code));
    }
    const HINTERNET handle =
        WinHttpOpenRequest(op->connect, widen(http_method_name(request.method)).c_str(),
                           widen(parts.target).c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (handle == nullptr) {
        const DWORD code = GetLastError();
        (void)WinHttpCloseHandle(op->connect);
        return report(
            failure(error_kind_for_winhttp_status(code), "WinHttpOpenRequest failed", code));
    }

    // Nothing is followed here, no cookie is kept: the module owns redirects and the request's
    // headers are the caller's.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    (void)WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    (void)WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    // A safety net only, set past the module's own deadline so the module's `timeout` wins.
    if (request.timeout_ms != 0) {
        const int net = static_cast<int>(request.timeout_ms) + 1000;
        (void)WinHttpSetTimeouts(handle, net, net, net, net);
    }

    op->hook->request = handle;
    Op* const raw = op.release();
    {
        std::lock_guard lock(impl_->ops_mutex);
        impl_->ops.insert(raw);
        impl_->hooks.push_back(raw->hook);
    }
    (void)WinHttpSetStatusCallback(handle, &on_status, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
    raw->on_done = std::move(on_done);

    // After the op is registered: a token cancelled from here on closes the handle through the
    // closer. A token that was already cancelled ran the callback inline.
    Impl* const impl = impl_.get();
    raw->registration = token.on_cancel([impl, hook = raw->hook] {
        hook->cancelled.store(true);
        impl->request_close(hook);
    });

    const DWORD body_size = static_cast<DWORD>(raw->body_in.size());
    const BOOL sent = WinHttpSendRequest(
        handle,
        raw->wide_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : raw->wide_headers.c_str(),
        static_cast<DWORD>(raw->wide_headers.size()),
        body_size == 0 ? WINHTTP_NO_REQUEST_DATA : raw->body_in.data(), body_size, body_size,
        reinterpret_cast<DWORD_PTR>(raw));
    if (!sent) {
        fail(*raw, GetLastError());
    }
    return true;
}

} // namespace tessera::appfw
