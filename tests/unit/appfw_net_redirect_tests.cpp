// SPDX-License-Identifier: MIT
//
// The `net` redirect, header and URL rules (docs/specs/networking-v1.md section 3 items 2, 7, 8 and
// 9; issue #197). One translation unit per spec clause group, so a failure names the rule that
// broke.
//
// The redirect tests assert the *chain the back end saw* (`FakeNet::received()`), not only the
// final response: "POST became GET and the body was dropped" is a fact about the second request,
// not about the answer to it.

#include "appfw_net_test_support.hpp"

#include <string>
#include <string_view>

namespace net_test {

int run_redirect_tests();

namespace {

using appfw::HttpMethod;

// Section 3 item 2: "301, 302 and 303 change POST to GET and drop the body."
void a_post_becomes_a_get_on_301_302_303() {
    for (const int status : {301, 302, 303}) {
        Harness harness;
        harness.fake.push_response(redirect_to(status, "https://api.example/target"));
        harness.fake.push_response(response_with(200, "ok"));
        NetRequest request = request_for("https://api.example/source");
        request.method = HttpMethod::Post;
        request.body = "payload";
        request.has_body = true;
        const NetTicket ticket = submit(harness, request);
        pump(harness);

        expect(harness.fake.received().size() == 2, "one redirect means two requests");
        if (harness.fake.received().size() != 2) {
            continue;
        }
        expect(harness.fake.received()[1].method == HttpMethod::Get,
               "verbatim: 301/302/303 change POST to GET");
        expect(!harness.fake.received()[1].has_body, "verbatim: 301/302/303 drop the body");
        expect(harness.fake.received()[1].body.empty(), "the dropped body is not sent");
        expect(harness.client.status_of(ticket).outcome == appfw::NetOutcome::Ok,
               "the chain ends on the final response");
    }
}

// Section 3 item 2: "307 and 308 keep method and body."
void a_post_survives_307_308() {
    for (const int status : {307, 308}) {
        Harness harness;
        harness.fake.push_response(redirect_to(status, "https://api.example/target"));
        harness.fake.push_response(response_with(200, "ok"));
        NetRequest request = request_for("https://api.example/source");
        request.method = HttpMethod::Post;
        request.body = "payload";
        request.has_body = true;
        submit(harness, request);
        pump(harness);

        expect(harness.fake.received().size() == 2, "one redirect means two requests");
        if (harness.fake.received().size() != 2) {
            continue;
        }
        expect(harness.fake.received()[1].method == HttpMethod::Post,
               "verbatim: 307/308 keep the method");
        expect(harness.fake.received()[1].body == "payload", "verbatim: 307/308 keep the body");
    }
}

// Section 3 item 2: "An Authorization header is dropped when a redirect changes the origin."
void a_cross_origin_redirect_drops_every_credential() {
    Harness harness;
    harness.fake.push_response(redirect_to(302, "https://other.example/target"));
    harness.fake.push_response(response_with(200, "ok"));
    NetRequest request = request_for("https://api.example/source");
    request.headers.emplace_back("Authorization", "Bearer secret");
    request.headers.emplace_back("Cookie", "session=secret");
    request.headers.emplace_back("Proxy-Authorization", "Basic secret");
    request.headers.emplace_back("Accept", "application/json");
    submit(harness, request);
    pump(harness);

    expect(harness.fake.received().size() == 2, "the redirect was followed");
    if (harness.fake.received().size() != 2) {
        return;
    }
    // Every credential reached the first origin...
    for (const char* header : {"authorization", "cookie", "proxy-authorization"}) {
        expect(has_header(harness.fake.received()[0].headers, header),
               "the first request carried the credential");
    }
    // ...and none reached the second. A cookie matters most in practice: it is ambient, so an app
    // that never sets `Authorization` still leaks its session if this list is incomplete.
    for (const char* header : {"authorization", "cookie", "proxy-authorization"}) {
        expect(!has_header(harness.fake.received()[1].headers, header),
               "verbatim: a cross-origin redirect drops every credential");
    }
    // A non-credential header is untouched: the rule is about credentials, not about stripping.
    expect(has_header(harness.fake.received()[1].headers, "accept"),
           "a non-credential header survives the cross-origin hop");
}

// The same origin under a different spelling is *not* a cross-origin hop: the port is the effective
// one, so an explicit `:443` does not count as a change.
void a_same_origin_redirect_keeps_credentials() {
    Harness harness;
    harness.fake.push_response(redirect_to(302, "https://API.example:443/target"));
    harness.fake.push_response(response_with(200, "ok"));
    NetRequest request = request_for("https://api.example/source");
    request.headers.emplace_back("Authorization", "Bearer secret");
    request.headers.emplace_back("Cookie", "session=secret");
    submit(harness, request);
    pump(harness);

    expect(harness.fake.received().size() == 2, "the redirect was followed");
    if (harness.fake.received().size() != 2) {
        return;
    }
    expect(has_header(harness.fake.received()[1].headers, "authorization"),
           "a same-origin redirect keeps the credential, even spelled with the default port and a "
           "different case");
    expect(has_header(harness.fake.received()[1].headers, "cookie"),
           "and keeps the cookie, which is scoped to the same origin");
}

// Section 3 item 2: "An https to http redirect is refused with unsupported."
void an_https_to_http_redirect_is_refused() {
    Harness harness;
    harness.fake.push_response(redirect_to(302, "http://api.example/downgrade"));
    const NetTicket ticket = submit(harness, request_for("https://api.example/secure"));
    pump(harness);
    const auto status = harness.client.status_of(ticket);
    expect(status.state == appfw::NetRequestState::Error, "a downgrade ends the request");
    expect(status.error_kind == NetErrorKind::Unsupported,
           "verbatim: an https to http redirect is refused with unsupported");
    expect(harness.fake.received().size() == 1, "the downgraded request was never sent");
}

// `manual` hands the 3xx back as a response: the caller decides, as section 2's field says.
void a_manual_redirect_is_a_response() {
    Harness harness;
    harness.fake.push_response(redirect_to(302, "https://api.example/target"));
    NetRequest request = request_for("https://api.example/source");
    request.redirect = RedirectPolicy::Manual;
    const NetTicket ticket = submit(harness, request);
    pump(harness);
    const auto status = harness.client.status_of(ticket);
    expect(status.state == appfw::NetRequestState::Done,
           "a manual redirect is a completed request");
    expect(status.outcome == appfw::NetOutcome::HttpStatus, "a 302 is the HttpStatus outcome");
    expect(status.status == 302, "the caller sees the 302 itself");
    expect(harness.fake.received().size() == 1, "no second request was made");
}

// A status that is not one of the five section 3 item 2 names is not followed either.
void an_unlisted_3xx_is_a_response() {
    Harness harness;
    harness.fake.push_response(redirect_to(304, "https://api.example/cached"));
    const NetTicket ticket = submit(harness, request_for("https://api.example/etag"));
    pump(harness);
    expect(harness.client.status_of(ticket).outcome == appfw::NetOutcome::HttpStatus,
           "a 304 is a response, not a redirect to follow");
    expect(harness.fake.received().size() == 1, "the 304 was not followed");
}

// A relative target needs RFC 3986 resolution, which this slice does not implement; it is refused
// as `unsupported` rather than guessed (net_url.hpp states the gap).
void a_relative_redirect_is_unsupported() {
    Harness harness;
    harness.fake.push_response(redirect_to(302, "/relative"));
    const NetTicket ticket = submit(harness, request_for("https://api.example/source"));
    pump(harness);
    expect(harness.client.status_of(ticket).error_kind == NetErrorKind::Unsupported,
           "a relative redirect target is unsupported, not a guessed URL");
}

// Section 3 item 7: the module adds the fixed `Tessera/<version>` and nothing else, and a caller's
// own User-Agent is never overwritten.
void the_default_user_agent_is_the_only_added_header() {
    Harness harness;
    harness.fake.push_response(response_with(200));
    submit(harness, request_for("https://api.example/x"));
    pump(harness);
    expect(harness.fake.received().size() == 1, "the request reached the back end");
    if (harness.fake.received().empty()) {
        return;
    }
    const NetHeaders& headers = harness.fake.received()[0].headers;
    const std::string* const agent = find_header(headers, "user-agent");
    expect(agent != nullptr, "the module sends a User-Agent");
    expect(agent != nullptr && agent->rfind("Tessera/", 0) == 0,
           "verbatim: the added User-Agent is Tessera/<version>");
    expect(headers.size() == 1,
           "the module added no cookie, cache or Accept-Encoding header of its own");

    Harness own;
    own.fake.push_response(response_with(200));
    NetRequest request = request_for("https://api.example/x");
    request.headers.emplace_back("User-Agent", "MyApp/1.0");
    submit(own, request);
    pump(own);
    expect(own.fake.received().size() == 1, "the request reached the back end");
    if (own.fake.received().empty()) {
        return;
    }
    const std::string* const mine = find_header(own.fake.received()[0].headers, "user-agent");
    expect(mine != nullptr && *mine == "MyApp/1.0",
           "a caller's own User-Agent is never replaced by the default");
}

// Section 3 item 8: plain http is allowed only when the app enables it.
void cleartext_is_refused_unless_enabled() {
    Harness refused;
    refused.fake.push_response(response_with(200));
    auto ticket = refused.client.submit(request_for("http://api.example/x"), {}, {});
    expect(!ticket.has_value(), "cleartext is refused by default");
    expect(ticket.error().kind == NetErrorKind::Unsupported,
           "verbatim: cleartext without net.allowCleartext is unsupported");
    expect(refused.fake.received().empty(), "the cleartext request never reached the back end");

    NetClientConfig config;
    config.allow_cleartext = true;
    Harness allowed(config);
    allowed.fake.push_response(response_with(200, "ok"));
    auto accepted = allowed.client.submit(request_for("http://api.example/x"), {}, {});
    expect(accepted.has_value(), "cleartext is accepted when the app enables it");
    pump(allowed);
    expect(accepted.has_value() &&
               allowed.client.status_of(*accepted).state == appfw::NetRequestState::Done,
           "the enabled cleartext request completes");
}

// Section 3 item 9: a URL in a diagnostic drops the query string and the userinfo.
void diagnostics_redact_the_query_string_and_userinfo() {
    using appfw::redacted_url;
    expect(redacted_url("https://user:pass@api.example/path?token=secret") ==
               "https://api.example/path",
           "verbatim: redaction removes both the userinfo and the query string");
    expect(redacted_url("https://api.example/path?token=secret") == "https://api.example/path",
           "the query string alone is removed");
    expect(redacted_url("not a url?token=secret") == "not a url",
           "an unparsable URL cannot smuggle its query string into a diagnostic");

    Harness harness;
    harness.fake.push_error(NetErrorKind::Connect, "refused", 0);
    const NetTicket ticket = submit(harness, request_for("https://api.example/x?token=secret"));
    pump(harness);
    // The response's final_url keeps the query (it is what was sent); only diagnostics redact it.
    expect(harness.client.status_of(ticket).error_kind == NetErrorKind::Connect,
           "the failure is the transport's");
}

// Section 2's `bad_url` is a named outcome, not an `other`, and it is refused before any I/O.
void a_bad_url_is_refused_before_any_io() {
    Harness harness;
    for (const std::string_view url :
         {"", "api.example/x", "ftp://api.example/x", "https://", "https://api.example:99999/x"}) {
        auto ticket = harness.client.submit(request_for(std::string(url)), {}, {});
        expect(!ticket.has_value(), "a malformed URL is refused at submit");
        if (!ticket.has_value()) {
            expect(ticket.error().kind == NetErrorKind::BadUrl ||
                       ticket.error().kind == NetErrorKind::Unsupported,
                   "a malformed URL is bad_url; an unimplemented scheme is unsupported");
        }
    }
    expect(harness.fake.received().empty(), "no malformed URL reached the back end");
    expect(!harness.executor.run_one(), "nothing was queued on the I/O executor either");
}

// Security review finding S7: `\` is not legal in a URI authority, and Windows HTTP stacks
// normalise it to `/`, so `good.example\@evil.example` displays as one host and resolves to
// another. A URL is read by a human and parsed by a machine, so the two must agree.
void a_backslash_in_the_authority_is_refused() {
    Harness harness;
    auto ticket =
        harness.client.submit(request_for("https://good.example\\@evil.example/x"), {}, {});
    expect(!ticket.has_value(), "a backslash in the authority is refused at submit");
    if (!ticket.has_value()) {
        expect(ticket.error().kind == NetErrorKind::BadUrl,
               "and it is bad_url, not a transport failure");
    }
    // A backslash in the *path* is not an authority character, so the rule must not catch it --
    // otherwise the fix would reject URLs that never had the problem.
    Harness path_harness;
    path_harness.fake.push_response(response_with(200, "ok"));
    auto path_ticket = path_harness.client.submit(request_for("https://good.example/a\\b"), {}, {});
    expect(path_ticket.has_value(), "a backslash after the authority is left alone");
}

} // namespace

int run_redirect_tests() {
    a_post_becomes_a_get_on_301_302_303();
    a_post_survives_307_308();
    a_cross_origin_redirect_drops_every_credential();
    a_same_origin_redirect_keeps_credentials();
    an_https_to_http_redirect_is_refused();
    a_manual_redirect_is_a_response();
    an_unlisted_3xx_is_a_response();
    a_relative_redirect_is_unsupported();
    the_default_user_agent_is_the_only_added_header();
    cleartext_is_refused_unless_enabled();
    diagnostics_redact_the_query_string_and_userinfo();
    a_bad_url_is_refused_before_any_io();
    a_backslash_in_the_authority_is_refused();
    return g_failures;
}

} // namespace net_test
