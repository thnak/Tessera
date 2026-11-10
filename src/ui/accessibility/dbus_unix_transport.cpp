// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/dbus_transport.hpp"

#if defined(__linux__)

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tessera::ui::a11y {
namespace {

// Plain SOCK_STREAM UNIX socket. The bridge only ever calls this from its own thread, so a blocking
// connect is acceptable there (it is not the UI thread, ADR-021); every read waits through poll()
// with the caller's bounded timeout, and every write loops until the byte span is out.
class UnixSocketTransport final : public Transport {
public:
    ~UnixSocketTransport() override {
        close();
    }

    [[nodiscard]] std::expected<void, TransportError> connect(const BusAddress& address) override {
        if (fd_ >= 0) {
            return {};
        }
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::unexpected(TransportError::ConnectFailed);
        }
        sockaddr_un endpoint{};
        endpoint.sun_family = AF_UNIX;
        socklen_t length = 0;
        constexpr std::size_t kSunPathBytes = sizeof(endpoint.sun_path);
        if (address.path.size() >= kSunPathBytes) {
            ::close(fd);
            return std::unexpected(TransportError::TooLarge);
        }
        if (address.abstract()) {
            // Abstract names start with a NUL and are not NUL-terminated; the address length tells
            // the kernel where the name ends.
            endpoint.sun_path[0] = '\0';
            std::memcpy(endpoint.sun_path + 1, address.path.data(), address.path.size());
            length =
                static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + address.path.size());
        } else {
            std::memcpy(endpoint.sun_path, address.path.c_str(), address.path.size() + 1);
            length =
                static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address.path.size() + 1);
        }
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), length) != 0) {
            ::close(fd);
            return std::unexpected(TransportError::ConnectFailed);
        }
        fd_ = fd;
        return {};
    }

    [[nodiscard]] std::expected<std::size_t, TransportError> read_some(std::span<std::uint8_t> out,
                                                                       int timeout_ms) override {
        if (out.empty()) {
            return 0;
        }
        if (fd_ < 0) {
            return std::unexpected(TransportError::NotConnected);
        }
        pollfd entry{fd_, POLLIN, 0};
        int ready = 0;
        do {
            ready = ::poll(&entry, 1, timeout_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            return std::unexpected(TransportError::IoError);
        }
        if (ready == 0) {
            return std::unexpected(TransportError::WouldBlock);
        }
        const ssize_t count = ::recv(fd_, out.data(), out.size(), 0);
        if (count > 0) {
            return static_cast<std::size_t>(count);
        }
        if (count == 0) {
            close();
            return std::unexpected(TransportError::Closed);
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::unexpected(TransportError::WouldBlock);
        }
        return std::unexpected(TransportError::IoError);
    }

    [[nodiscard]] std::expected<void, TransportError>
    write_all(std::span<const std::uint8_t> bytes) override {
        if (fd_ < 0) {
            return std::unexpected(TransportError::NotConnected);
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            // MSG_NOSIGNAL: a peer that goes away must surface as an error value, never as SIGPIPE.
            const ssize_t written =
                ::send(fd_, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(TransportError::IoError);
            }
            if (written == 0) {
                return std::unexpected(TransportError::Closed);
            }
            offset += static_cast<std::size_t>(written);
        }
        return {};
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] bool connected() const override {
        return fd_ >= 0;
    }

private:
    int fd_ = -1;
};

} // namespace

std::unique_ptr<Transport> create_unix_socket_transport() {
    return std::make_unique<UnixSocketTransport>();
}

} // namespace tessera::ui::a11y

#else

namespace tessera::ui::a11y {

// No UNIX-domain sockets on this platform (Windows uses its own bus transport, out of scope for the
// Linux AT-SPI2 bridge). The bridge treats nullptr as "accessibility unavailable".
std::unique_ptr<Transport> create_unix_socket_transport() {
    return nullptr;
}

} // namespace tessera::ui::a11y

#endif
