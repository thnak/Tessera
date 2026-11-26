// SPDX-License-Identifier: MIT
//
// `CurlTransport` (issue #836). See net_curl.hpp for the contract; this file is the driver loop.
//
// **Threads.** One driver thread per transport owns the `CURLM` and every easy handle. `start()`
// (any thread) builds an `Op`, queues it and calls `curl_multi_wakeup`; a token's cancel callback
// sets a flag and wakes the driver the same way, so no libcurl call is made from a foreign thread
// except the wakeup, which is the one libcurl documents as thread-safe. The thread is joined in the
// destructor; nothing is detached.
//
// **Lifetime.** An `Op` belongs to the driver from the moment it is queued until its result is
// posted. The token callback holds only the shared cancel flag, never the op.

#include "tessera/appfw/net_curl.hpp"
#include "tessera/appfw/net_curl_map.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tessera::appfw {

// The mapping's constants are plain integers so a lane without libcurl can test the table. These
// make a transcription mistake a build failure here.
static_assert(kCurlUnsupportedProtocol == CURLE_UNSUPPORTED_PROTOCOL);
static_assert(kCurlUrlMalformat == CURLE_URL_MALFORMAT);
static_assert(kCurlCouldntResolveProxy == CURLE_COULDNT_RESOLVE_PROXY);
static_assert(kCurlCouldntResolveHost == CURLE_COULDNT_RESOLVE_HOST);
static_assert(kCurlCouldntConnect == CURLE_COULDNT_CONNECT);
static_assert(kCurlWriteError == CURLE_WRITE_ERROR);
static_assert(kCurlOperationTimedout == CURLE_OPERATION_TIMEDOUT);
static_assert(kCurlSslConnectError == CURLE_SSL_CONNECT_ERROR);
static_assert(kCurlAbortedByCallback == CURLE_ABORTED_BY_CALLBACK);
static_assert(kCurlPeerFailedVerification == CURLE_PEER_FAILED_VERIFICATION);
static_assert(kCurlSslCertproblem == CURLE_SSL_CERTPROBLEM);
static_assert(kCurlSslCipher == CURLE_SSL_CIPHER);
static_assert(kCurlSslCacertBadfile == CURLE_SSL_CACERT_BADFILE);
static_assert(kCurlSslIssuerError == CURLE_SSL_ISSUER_ERROR);

namespace {

struct Op {
    CurlTransport::Completion on_done;
    NetRequest request;
    std::string body_in;
    CURL* easy = nullptr;
    curl_slist* headers = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    runtime::CancellationRegistration registration;
    NetResponse response;
    std::string body;
    bool too_large = false;
    char error[CURL_ERROR_SIZE] = {};
};

std::size_t on_write(char* data, std::size_t size, std::size_t count, void* user) {
    auto* const op = static_cast<Op*>(user);
    const std::size_t bytes = size * count;
    // Section 3 item 5: stop reading rather than buffer past the cap. Returning less than `bytes`
    // aborts the transfer with CURLE_WRITE_ERROR, which the result maps back to too_large.
    if (op->request.max_response_bytes != 0 &&
        op->body.size() + bytes > op->request.max_response_bytes) {
        op->too_large = true;
        return 0;
    }
    op->body.append(data, bytes);
    return bytes;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user) {
    auto* const op = static_cast<Op*>(user);
    const std::size_t bytes = size * count;
    const std::string_view line = trim(std::string_view(data, bytes));
    if (line.rfind("HTTP/", 0) == 0) {
        op->response.headers.clear(); // a new status line: an interim or earlier block is dropped
        return bytes;
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && colon > 0) {
        op->response.headers.emplace_back(std::string(trim(line.substr(0, colon))),
                                          std::string(trim(line.substr(colon + 1))));
    }
    return bytes;
}

// The header list: the caller's, in the caller's order. libcurl adds Accept, Expect, User-Agent and
// (for a body) Content-Type by itself; the seam says the transport adds nothing, so each is
// suppressed unless the caller sent it. A value-less header is sent as `Name;`, curl's spelling.
curl_slist* build_headers(const NetRequest& request) {
    curl_slist* list = nullptr;
    for (const auto& [name, value] : request.headers) {
        const std::string line = value.empty() ? name + ";" : name + ": " + value;
        list = curl_slist_append(list, line.c_str());
    }
    constexpr std::string_view suppressed[] = {"Accept", "Expect", "User-Agent", "Content-Type"};
    for (const std::string_view name : suppressed) {
        if (!has_header(request.headers, name)) {
            list = curl_slist_append(list, (std::string(name) + ":").c_str());
        }
    }
    return list;
}

NetProtocol protocol_of(long version) {
    switch (version) {
    case CURL_HTTP_VERSION_2_0:
        return NetProtocol::H2;
#ifdef CURL_HTTP_VERSION_3
    case CURL_HTTP_VERSION_3:
        return NetProtocol::H3;
#endif
    case CURL_HTTP_VERSION_1_0:
    case CURL_HTTP_VERSION_1_1:
        return NetProtocol::H1;
    default:
        return NetProtocol::Unknown;
    }
}

} // namespace

struct CurlTransport::Impl {
    explicit Impl(runtime::CompletionQueue& queue)
        : completions(&queue) {}

    runtime::CompletionQueue* completions;
    CURLM* multi = nullptr;
    std::thread driver;

    std::mutex mutex;
    std::deque<Op*> incoming;
    bool stop = false;
    // Driver-thread only.
    std::unordered_set<Op*> active;

    void wake() {
        if (multi != nullptr) {
            (void)curl_multi_wakeup(multi);
        }
    }

    void post(Op& op, NetResult result) {
        (void)completions->post(
            runtime::CompletionPriority::Visible,
            [done = std::move(op.on_done), result = std::move(result)]() mutable {
                done(std::move(result));
            });
    }

    // Reports and releases one op. `code` is the CURLcode of its transfer.
    void finish(Op* op, CURLcode code) {
        (void)curl_multi_remove_handle(multi, op->easy);
        NetResult result;
        if (op->cancelled->load()) {
            result =
                std::unexpected(NetError{NetErrorKind::Cancelled, "the request was cancelled", 0});
        } else if (op->too_large) {
            result = std::unexpected(NetError{NetErrorKind::TooLarge,
                                              "the response body exceeds max_response_bytes", 0});
        } else if (code != CURLE_OK) {
            const std::string detail = op->error[0] != '\0' ? op->error : curl_easy_strerror(code);
            result =
                std::unexpected(NetError{error_kind_for_curl_code(static_cast<std::int32_t>(code)),
                                         detail, static_cast<std::int64_t>(code)});
        } else {
            long status = 0;
            long version = 0;
            (void)curl_easy_getinfo(op->easy, CURLINFO_RESPONSE_CODE, &status);
            (void)curl_easy_getinfo(op->easy, CURLINFO_HTTP_VERSION, &version);
            op->response.status = static_cast<int>(status);
            op->response.protocol = protocol_of(version);
            op->response.final_url = op->request.url;
            op->response.body = std::move(op->body);
            result = NetResult{std::move(op->response)};
        }
        post(*op, std::move(result));
        active.erase(op);
        // The registration is torn down here, on the driver: its callback only sets a flag and
        // wakes this thread, so waiting for a running one cannot deadlock.
        op->registration.reset();
        curl_easy_cleanup(op->easy);
        curl_slist_free_all(op->headers);
        delete op;
    }

    void run() {
        for (;;) {
            std::deque<Op*> fresh;
            bool stopping = false;
            {
                std::lock_guard lock(mutex);
                fresh.swap(incoming);
                stopping = stop;
            }
            for (Op* op : fresh) {
                if (curl_multi_add_handle(multi, op->easy) != CURLM_OK) {
                    post(*op, std::unexpected(NetError{NetErrorKind::Other,
                                                       "libcurl refused the transfer", 0}));
                    op->registration.reset();
                    curl_easy_cleanup(op->easy);
                    curl_slist_free_all(op->headers);
                    delete op;
                    continue;
                }
                active.insert(op);
            }
            // A cancelled transfer is removed here, which closes its connection. On shutdown every
            // transfer is cancelled so the loop can end.
            std::vector<Op*> doomed;
            for (Op* op : active) {
                if (stopping) {
                    op->cancelled->store(true);
                }
                if (op->cancelled->load()) {
                    doomed.push_back(op);
                }
            }
            for (Op* op : doomed) {
                finish(op, CURLE_ABORTED_BY_CALLBACK);
            }

            int running = 0;
            (void)curl_multi_perform(multi, &running);
            int pending = 0;
            while (CURLMsg* message = curl_multi_info_read(multi, &pending)) {
                if (message->msg != CURLMSG_DONE) {
                    continue;
                }
                Op* op = nullptr;
                (void)curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &op);
                if (op != nullptr && active.count(op) != 0) {
                    finish(op, message->data.result);
                }
            }
            if (stopping && active.empty()) {
                std::lock_guard lock(mutex);
                if (incoming.empty()) {
                    return;
                }
            }
            (void)curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
        }
    }
};

CurlTransport::CurlTransport(runtime::CompletionQueue& completions)
    : impl_(std::make_unique<Impl>(completions)) {
    // Not thread-safe by libcurl's own rule, so exactly once for the process.
    static std::once_flag once;
    std::call_once(once, [] { (void)curl_global_init(CURL_GLOBAL_DEFAULT); });
    impl_->multi = curl_multi_init();
    if (impl_->multi != nullptr) {
        impl_->driver = std::thread([impl = impl_.get()] { impl->run(); });
    }
}

CurlTransport::~CurlTransport() {
    if (impl_ == nullptr) {
        return;
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->wake();
    if (impl_->driver.joinable()) {
        impl_->driver.join();
    }
    if (impl_->multi != nullptr) {
        (void)curl_multi_cleanup(impl_->multi);
    }
}

bool CurlTransport::available() const noexcept {
    return impl_ != nullptr && impl_->multi != nullptr;
}

std::string_view CurlTransport::name() const noexcept {
    return "curl";
}

bool CurlTransport::start(const NetRequest& request, runtime::CancellationToken token,
                          Completion on_done) {
    if (!available()) {
        return false;
    }
    auto op = std::make_unique<Op>();
    op->request = request;
    op->body_in = request.has_body ? request.body : std::string();
    op->on_done = std::move(on_done);
    if (token.cancelled()) {
        impl_->post(*op, std::unexpected(
                             NetError{NetErrorKind::Cancelled, "the request was cancelled", 0}));
        return true;
    }
    op->easy = curl_easy_init();
    if (op->easy == nullptr) {
        return false;
    }
    CURL* const easy = op->easy;
    op->headers = build_headers(request);
    (void)curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
    (void)curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    // Nothing is followed here: the module owns redirects.
    (void)curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    (void)curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    (void)curl_easy_setopt(easy, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    (void)curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
    // Verification is always on and has no off switch (section 3 item 8).
    (void)curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    (void)curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    if (request.method == HttpMethod::Head) {
        (void)curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    } else if (request.method != HttpMethod::Get || request.has_body) {
        (void)curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST,
                               std::string(http_method_name(request.method)).c_str());
    }
    const bool sends_body = request.method == HttpMethod::Post ||
                            request.method == HttpMethod::Put ||
                            request.method == HttpMethod::Patch || request.has_body;
    if (sends_body && request.method != HttpMethod::Head) {
        (void)curl_easy_setopt(easy, CURLOPT_POSTFIELDS, op->body_in.c_str());
        (void)curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                               static_cast<curl_off_t>(op->body_in.size()));
    }
    (void)curl_easy_setopt(easy, CURLOPT_HTTPHEADER, op->headers);
    // A safety net only, set past the module's own deadline so the module's `timeout` wins.
    if (request.timeout_ms != 0) {
        (void)curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                               static_cast<long>(request.timeout_ms) + 1000L);
    }
    (void)curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &on_write);
    (void)curl_easy_setopt(easy, CURLOPT_WRITEDATA, op.get());
    (void)curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &on_header);
    (void)curl_easy_setopt(easy, CURLOPT_HEADERDATA, op.get());
    (void)curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, op->error);
    (void)curl_easy_setopt(easy, CURLOPT_PRIVATE, op.get());

    Impl* const impl = impl_.get();
    op->registration = token.on_cancel([impl, flag = op->cancelled] {
        flag->store(true);
        impl->wake();
    });
    {
        std::lock_guard lock(impl_->mutex);
        impl_->incoming.push_back(op.release());
    }
    impl_->wake();
    return true;
}

} // namespace tessera::appfw
