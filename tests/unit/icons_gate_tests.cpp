// SPDX-License-Identifier: MIT
//
// Gate contract for `tessera icons add` (ADR-047 item 8): one case per license class through the
// whole import (so the message the CLI prints is what is asserted), the share-alike opt-in, the
// reference errors, and the static-icon subset that keeps the converter from drawing something
// different from the icon.

#include "icon_import.hpp"
#include "icon_svg.hpp"
#include "icons_test_support.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace icons = tessera::icons;
using namespace tessera::icons::test_support;

struct Rejection {
    std::string reference;
    std::string license;
    std::string reason;
};

// The whole import must fail, and the message must name the set, quote the license as recorded and
// carry the reason.
[[nodiscard]] bool rejects(const Rejection& rejection, bool allow_share_alike = false) {
    icons::ImportRequest request = fixture_request({rejection.reference});
    request.allow_share_alike = allow_share_alike;
    const auto result = icons::import_icons(request);
    if (result) {
        return expect(false, "a rejected license was imported: " + rejection.reference);
    }
    bool ok = true;
    ok = expect(contains(result.error(), "cannot import icon set"),
                "the rejection does not name the set: " + result.error()) &&
         ok;
    ok = expect(contains(result.error(), rejection.license),
                "the rejection does not quote the license: " + result.error()) &&
         ok;
    ok = expect(contains(result.error(), rejection.reason),
                "the rejection does not carry the reason: " + result.error()) &&
         ok;
    return ok;
}

[[nodiscard]] bool rejects_body(std::string_view body, std::string_view expected) {
    icons::IconSource source;
    source.name = "bad";
    source.width = 24;
    source.height = 24;
    source.body = std::string(body);
    const auto svg = icons::canonical_icon_svg(source, icons::SvgOptions{});
    if (svg) {
        return expect(false, "a body outside the static-icon subset was converted");
    }
    return expect(contains(svg.error(), expected),
                  "the subset diagnostic is unhelpful: " + svg.error());
}

} // namespace

int main() {
    bool ok = true;

    // One case per rejected class, through the full import.
    ok = rejects({"gpl3:copyleft", "GPL-3.0", "GPL-family"}) && ok;
    ok = rejects({"nc:coin", "CC-BY-NC-4.0", "non-commercial"}) && ok;
    ok = rejects({"mpl:file", "MPL-2.0", "owner decision"}) && ok;
    ok = rejects({"unknown:mystery", "none recorded", "records no license"}) && ok;

    // A missing set is an IO failure, not a license failure; it still names the set.
    const auto missing_set = icons::import_icons(fixture_request({"no-such-set:mystery"}));
    ok = expect(!missing_set && contains(missing_set.error(), "cannot read") &&
                    contains(missing_set.error(), "no-such-set"),
                "a missing set does not name it") &&
         ok;

    // Share-alike needs the explicit opt-in; other accepted classes need no flag.
    ok = rejects({"sharealike:share", "CC-BY-SA-4.0", "share-alike"}) && ok;
    ok = expect(icons::import_icons(fixture_request({"sharealike:share"})).has_value() == false,
                "a share-alike set was imported without the opt-in") &&
         ok;
    icons::ImportRequest share_alike = fixture_request({"sharealike:share"});
    share_alike.allow_share_alike = true;
    const auto share_alike_result = icons::import_icons(share_alike);
    ok = expect(share_alike_result.has_value(), "a share-alike set was rejected with the opt-in") &&
         ok;
    if (share_alike_result) {
        const auto attribution = find_file(*share_alike_result, "ICON_LICENSES.txt");
        ok = expect(attribution != nullptr && contains(attribution->bytes, "License: CC-BY-SA-4.0"),
                    "share-alike attribution is missing the license") &&
             ok;
    }
    for (const std::string reference : {"bsd3:shield", "ccby:chart"}) {
        ok = expect(icons::import_icons(fixture_request({reference})).has_value(),
                    "an allowlisted license was rejected: " + reference) &&
             ok;
    }

    // Reference errors name what is wrong.
    const auto missing = icons::import_icons(fixture_request({"tabler:nope"}));
    ok =
        expect(!missing && contains(missing.error(), "tabler") && contains(missing.error(), "nope"),
               "a missing icon does not name the reference") &&
        ok;
    const auto malformed = icons::import_icons(fixture_request({"tabler"}));
    ok = expect(!malformed && contains(malformed.error(), "<set>:<name>"),
                "a malformed reference is accepted") &&
         ok;
    ok = expect(!icons::import_icons(fixture_request({})), "an empty reference list is accepted") &&
         ok;

    // The static-icon subset is enforced at conversion time, not by drawing something different.
    ok = rejects_body("<text x=\"0\" y=\"0\">A</text>", "static-icon subset") && ok;
    ok = rejects_body("<path class=\"cls\" d=\"M0 0h24v24z\" />", "CSS class or style") && ok;
    ok = rejects_body("<path style=\"fill:red\" d=\"M0 0h24v24z\" />", "CSS class or style") && ok;
    ok = rejects_body("<path fill=\"url(#grad)\" d=\"M0 0h24v24z\" />", "href/url") && ok;
    ok = rejects_body("<image href=\"a.png\" />", "static-icon subset") && ok;
    ok = rejects_body("<path d=\"M0 0h24v24z\"", "unterminated") && ok;
    ok = rejects_body("", "empty") && ok;

    // A plain filled path is inside the subset, and currentColor survives conversion.
    icons::IconSource good;
    good.name = "ok";
    good.width = 16;
    good.height = 16;
    good.body = "<path fill=\"currentColor\" d=\"M0 0h16v16z\" />";
    const auto svg = icons::canonical_icon_svg(good, icons::SvgOptions{});
    ok = expect(svg.has_value(), "a plain filled path was rejected") && ok;
    if (svg) {
        ok = expect(contains(*svg, "width=\"16\"") && contains(*svg, "viewBox=\"0 0 16 16\""),
                    "the envelope does not use the icon size") &&
             ok;
        ok = expect(contains(*svg, "fill=\"currentColor\""), "currentColor was rewritten") && ok;
    }

    if (!ok) {
        return 1;
    }
    std::cout << "icons_gate: ok\n";
    return 0;
}
