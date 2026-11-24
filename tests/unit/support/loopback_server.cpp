// SPDX-License-Identifier: MIT
//
// The loopback HTTP server (issue #882). See `loopback_server.hpp` for why it is hand-written.
//
// **Platform.** Sockets are the one place a test helper cannot be pure C++: this uses Winsock on
// Windows and BSD sockets elsewhere, with the two differences (initialisation and the close call)
// isolated in the two small helpers below. Everything else is shared.

#include "loopback_server.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#ifdef _WIN32
// `min`/`max` from <windows.h> would break `std::min`/`std::max` (MSVC's own headers define them as
// macros unless this is set first).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tessera::net_test {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

// Winsock needs process-wide initialisation, and a static object is the one place that can do it
// before the first server starts without every test calling it.
struct WinsockInit {
    WinsockInit() {
        WSADATA data{};
        (void)WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() {
        WSACleanup();
    }
};

void ensure_sockets() {
    static WinsockInit init;
    (void)init;
}

// Winsock spells "both directions" `SD_BOTH`; POSIX spells it `SHUT_RDWR`.
constexpr int kShutdownBoth = SD_BOTH;

void close_socket(SocketHandle handle) {
    (void)closesocket(handle);
}

#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void ensure_sockets() {}

constexpr int kShutdownBoth = SHUT_RDWR;

void close_socket(SocketHandle handle) {
    (void)::close(handle);
}

#endif

std::string lowercase(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Reads until `"\r\n\r\n"`, which is the end of an HTTP request's head. Returns false when the peer
// closed first or the head is larger than the buffer, which is a test bug rather than a server
// condition.
bool read_head(SocketHandle fd, std::string& out) {
    out.clear();
    std::array<char, 1024> buffer{};
    for (;;) {
        const auto received = ::recv(fd, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            return false;
        }
        out.append(buffer.data(), static_cast<std::size_t>(received));
        if (out.find("\r\n\r\n") != std::string::npos) {
            return true;
        }
        if (out.size() > 64 * 1024) {
            return false;
        }
    }
}

// Parses a request head plus whatever body arrived with it. Only `Content-Length` is honoured: the
// tests send plain bodies, and a chunked request would be a back end doing something the contract
// does not ask for.
LoopbackRequest parse_request(std::string_view head, int fd) {
    LoopbackRequest request;
    const std::size_t line_end = head.find("\r\n");
    const std::string_view request_line = head.substr(0, line_end);
    const std::size_t first_space = request_line.find(' ');
    if (first_space != std::string_view::npos) {
        request.method = std::string(request_line.substr(0, first_space));
        const std::size_t second_space = request_line.find(' ', first_space + 1);
        request.target =
            std::string(request_line.substr(first_space + 1, second_space == std::string_view::npos
                                                                 ? std::string_view::npos
                                                                 : second_space - first_space - 1));
    }
    std::size_t cursor = line_end == std::string_view::npos ? head.size() : line_end + 2;
    std::size_t body_offset = head.size();
    while (cursor < head.size()) {
        const std::size_t end = head.find("\r\n", cursor);
        if (end == std::string_view::npos || end == cursor) {
            body_offset = std::min(head.size(), cursor + 2);
            break;
        }
        const std::string_view line = head.substr(cursor, end - cursor);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            request.headers.emplace_back(lowercase(trim(line.substr(0, colon))),
                                         std::string(trim(line.substr(colon + 1))));
        }
        cursor = end + 2;
        body_offset = cursor;
    }
    if (body_offset < head.size()) {
        request.body = std::string(head.substr(body_offset));
    }
    // A body announced but not yet arrived is read here, so a back end that sends the head and the
    // body in two writes is not a flake.
    const std::string length_text = request.header("content-length");
    if (!length_text.empty()) {
        const std::size_t wanted = static_cast<std::size_t>(std::stoul(length_text));
        std::array<char, 4096> buffer{};
        while (request.body.size() < wanted) {
            const auto received = ::recv(fd, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0) {
                break;
            }
            request.body.append(buffer.data(), static_cast<std::size_t>(received));
        }
    }
    return request;
}

std::string render_response(const LoopbackResponse& response) {
    std::string out =
        "HTTP/1.1 " + std::to_string(response.status) + " " + response.reason + "\r\n";
    for (const auto& header : response.headers) {
        out += header.first;
        out += ": ";
        out += header.second;
        out += "\r\n";
    }
    // Always announced, so a back end never has to guess whether the body is complete.
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    if (response.close) {
        out += "Connection: close\r\n";
    }
    out += "\r\n";
    out += response.body;
    return out;
}

bool send_all(SocketHandle fd, std::string_view text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const int result = static_cast<int>(
            ::send(fd, text.data() + sent, static_cast<int>(text.size() - sent), 0));
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

} // namespace

std::string LoopbackRequest::header(std::string_view name) const {
    const std::string wanted = lowercase(name);
    for (const auto& entry : headers) {
        if (entry.first == wanted) {
            return entry.second;
        }
    }
    return {};
}

LoopbackServer::~LoopbackServer() {
    stop();
}

bool LoopbackServer::start(std::vector<LoopbackResponse> script) {
    stop();
    ensure_sockets();

    listener_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener_ == static_cast<int>(kInvalidSocket)) {
        return false;
    }
    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                       sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0; // ephemeral: two test binaries may run at once
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(static_cast<SocketHandle>(listener_));
        listener_ = -1;
        return false;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close_socket(static_cast<SocketHandle>(listener_));
        listener_ = -1;
        return false;
    }
    port_ = ntohs(address.sin_port);
    if (::listen(listener_, 4) != 0) {
        close_socket(static_cast<SocketHandle>(listener_));
        listener_ = -1;
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        script_ = std::move(script);
        requests_.clear();
        served_ = 0;
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void LoopbackServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        // Still join a thread that has already stopped, so `stop` is idempotent and the destructor
        // never leaves a joinable thread behind.
        if (thread_.joinable()) {
            thread_.join();
        }
        return;
    }
    // Waking the accept: `shutdown` first, because closing a listening socket does NOT wake a
    // blocked `accept()` on Linux - the descriptor stays open for the blocked call and the thread
    // waits forever. Windows happens to return WSAENOTSOCK and hide the bug, so this is the one
    // place the two platforms genuinely differ. `shutdown` makes a blocked accept return
    // immediately on both; the close that follows releases the descriptor.
    if (listener_ >= 0) {
        (void)::shutdown(static_cast<SocketHandle>(listener_), kShutdownBoth);
        close_socket(static_cast<SocketHandle>(listener_));
        listener_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    // Wake any waiter so `wait_for_requests` does not sit out its whole timeout after a stop.
    arrival_.notify_all();
}

std::string LoopbackServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

std::vector<LoopbackRequest> LoopbackServer::requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
}

std::size_t LoopbackServer::request_count() const {
    std::lock_guard lock(mutex_);
    return requests_.size();
}

bool LoopbackServer::wait_for_requests(std::size_t count, int timeout_ms) {
    std::unique_lock lock(mutex_);
    return arrival_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this, count] { return requests_.size() >= count; });
}

void LoopbackServer::accept_loop() {
    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const SocketHandle client = ::accept(static_cast<SocketHandle>(listener_),
                                             reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client == kInvalidSocket) {
            // Either the listener was closed by `stop`, or the accept was interrupted. Both mean
            // the loop should look at the flag and leave.
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }
            continue;
        }
        serve_connection(static_cast<int>(client));
    }
}

void LoopbackServer::serve_connection(int fd) {
    const SocketHandle handle = static_cast<SocketHandle>(fd);
    std::string head;
    if (read_head(handle, head)) {
        LoopbackRequest request = parse_request(head, fd);
        LoopbackResponse response;
        {
            std::lock_guard lock(mutex_);
            if (!script_.empty()) {
                // Past the end of the script, the last entry repeats: a test that expects N
                // identical responses writes one, and a redirect chain writes each hop.
                const std::size_t index = std::min(served_, script_.size() - 1);
                response = script_[index];
                ++served_;
            }
            requests_.push_back(std::move(request));
        }
        arrival_.notify_all();
        (void)send_all(handle, render_response(response));
    }
    close_socket(handle);
}

} // namespace tessera::net_test
