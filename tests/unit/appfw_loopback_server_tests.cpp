// SPDX-License-Identifier: MIT
//
// The loopback server the net contract suite runs against (issue #882).
//
// **What these test.** The server is test infrastructure, so the risk is not "does HTTP work" but
// "does it fail in a way a back end test would misread". A server that returns 200 for a request it
// never recorded, or that reuses a script entry out of order, would let a contract test pass while
// asserting nothing. So the cases here are: a request arrives and is recorded, the script is
// consumed in order, the last entry repeats, headers are lowercased, a body is read, and a stop is
// clean.
//
// The client is a raw socket rather than a `NetTransport`: this file must not depend on the thing
// it tests, or a back end bug would look like a server bug.

#include "support/loopback_server.hpp"

#include <iostream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using tessera::net_test::LoopbackResponse;
using tessera::net_test::LoopbackServer;

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

#ifdef _WIN32
using RawSocket = SOCKET;
#else
using RawSocket = int;
#endif

void close_raw(RawSocket socket) {
#ifdef _WIN32
    (void)closesocket(socket);
#else
    (void)::close(socket);
#endif
}

// Sends `request` to the server and returns everything it sent back.
std::string talk(const LoopbackServer& server, std::string_view request) {
    RawSocket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_raw(socket);
        return {};
    }
    (void)::send(socket, request.data(), static_cast<int>(request.size()), 0);
    std::string response;
    char buffer[4096];
    for (;;) {
        const int received = static_cast<int>(::recv(socket, buffer, sizeof(buffer), 0));
        if (received <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }
    close_raw(socket);
    return response;
}

void test_a_request_is_recorded_and_answered() {
    LoopbackServer server;
    check(server.start({LoopbackResponse{.status = 200, .reason = "OK", .body = "hello"}}),
          "the server starts on an ephemeral port");
    check(server.running(), "and reports that it is running");

    const std::string response =
        talk(server, "GET /a/b?c=d HTTP/1.1\r\nHost: x\r\nX-Test: Value\r\n\r\n");
    check(response.find("200 OK") != std::string::npos, "the scripted status comes back");
    check(response.find("hello") != std::string::npos, "and the scripted body");

    check(server.wait_for_requests(1), "the request is recorded");
    const auto requests = server.requests();
    check(requests.size() == 1, "exactly one request arrived");
    check(requests[0].method == "GET", "the method is parsed");
    check(requests[0].target == "/a/b?c=d", "the target keeps the query");
    check(requests[0].header("x-test") == "Value",
          "a header is found regardless of case, because names are lowercased on record");
    server.stop();
}

void test_the_script_is_consumed_in_order_then_repeats_its_last_entry() {
    // A redirect chain needs each hop to be its own entry; a test that expects three identical
    // responses writes one. Getting this backwards would silently answer a chain with its first hop
    // forever, which looks like "the back end does not follow redirects" rather than a test bug.
    LoopbackServer server;
    check(server.start({
              LoopbackResponse{.status = 301,
                               .reason = "Moved",
                               .headers = {{"Location", "/two"}},
                               .body = {},
                               .close = false},
              LoopbackResponse{.status = 200, .reason = "OK", .headers = {}, .body = "final"},
          }),
          "the server starts with a two-entry script");

    const std::string first = talk(server, "GET /one HTTP/1.1\r\nHost: x\r\n\r\n");
    check(first.find("301 Moved") != std::string::npos, "the first request gets the first entry");
    const std::string second = talk(server, "GET /two HTTP/1.1\r\nHost: x\r\n\r\n");
    check(second.find("200 OK") != std::string::npos, "the second request gets the second entry");
    const std::string third = talk(server, "GET /three HTTP/1.1\r\nHost: x\r\n\r\n");
    check(third.find("200 OK") != std::string::npos, "past the end, the last entry repeats");
    check(server.wait_for_requests(3), "all three are recorded");
    server.stop();
}

void test_a_body_is_read() {
    // A back end that sends the head and the body in one write, and one that splits them, must both
    // work: the server reads Content-Length rather than assuming the head's packet ended the
    // request.
    LoopbackServer server;
    check(server.start(
              {LoopbackResponse{.status = 201, .reason = "Created", .headers = {}, .body = {}}}),
          "the server starts");
    const std::string response = talk(server, "POST /submit HTTP/1.1\r\nHost: x\r\nContent-Length: "
                                              "5\r\nContent-Type: text/plain\r\n\r\nabcde");
    check(response.find("201 Created") != std::string::npos, "the scripted status comes back");
    check(server.wait_for_requests(1), "the request is recorded");
    const auto requests = server.requests();
    check(!requests.empty() && requests[0].body == "abcde", "the body arrives whole");
    server.stop();
}

void test_no_script_answers_an_empty_200() {
    // A test that starts a server and forgets the script still gets a valid response, so the
    // failure it sees is the back end's, not a hang.
    LoopbackServer server;
    check(server.start({}), "the server starts with no script");
    const std::string response = talk(server, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    check(response.find("200 OK") != std::string::npos, "an empty script answers 200");
    check(server.wait_for_requests(1), "and still records the request");
    server.stop();
}

void test_stop_is_clean_and_idempotent() {
    LoopbackServer server;
    check(server.start({LoopbackResponse{}}), "the server starts");
    const std::uint16_t port = server.port();
    check(port != 0, "and has a port");
    server.stop();
    check(!server.running(), "stop clears running");
    server.stop(); // must not hang or crash
    check(!server.running(), "and a second stop is a no-op");

    // A stopped server's port is closed: a connect attempt fails, which is what tells a test the
    // server really went away rather than being left listening.
    RawSocket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool connected =
        ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    close_raw(socket);
    check(!connected, "a stopped server refuses connections");
}

void test_two_servers_do_not_collide() {
    // The port is ephemeral precisely so a test binary running twice, or two tests at once, does
    // not fight over a number.
    LoopbackServer first;
    LoopbackServer second;
    check(first.start({LoopbackResponse{}}), "the first server starts");
    check(second.start({LoopbackResponse{}}), "the second server starts");
    check(first.port() != second.port(), "and they have different ports");
    first.stop();
    second.stop();
}

} // namespace

int main() {
    test_a_request_is_recorded_and_answered();
    test_the_script_is_consumed_in_order_then_repeats_its_last_entry();
    test_a_body_is_read();
    test_no_script_answers_an_empty_200();
    test_stop_is_clean_and_idempotent();
    test_two_servers_do_not_collide();
    if (failures != 0) {
        std::cerr << "loopback_server: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "loopback_server: ok\n";
    return 0;
}
