// SPDX-License-Identifier: MIT
//
// License gate for the Iconify import (ADR-047 item 8, docs/design/component-library.md section
// 3.1): one case per license class, plus the rejection message contract (it must name the set, the
// license as recorded and the reason).

#include "icon_license.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::icons::LicenseClass;
using tessera::icons::LicenseGate;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "icons_license: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// One accepted identifier: the gate must pass and classify it as `license_class`.
[[nodiscard]] bool accepts(std::string_view identifier, LicenseClass license_class,
                           bool allow_share_alike = false) {
    const LicenseGate gate = tessera::icons::check_license("set", identifier, allow_share_alike);
    if (!gate.accepted) {
        std::cerr << "icons_license: \"" << identifier
                  << "\" should be accepted, reason: " << gate.reason << "\n";
        return false;
    }
    if (gate.license.license_class != license_class) {
        std::cerr << "icons_license: \"" << identifier << "\" classified differently\n";
        return false;
    }
    if (!gate.reason.empty()) {
        std::cerr << "icons_license: \"" << identifier << "\" accepted with a reason set\n";
        return false;
    }
    return true;
}

// One rejected identifier: the gate must fail, classify it as `license_class`, and the message must
// name the set, quote the license as recorded, and carry a reason.
[[nodiscard]] bool rejects(std::string_view set, std::string_view identifier,
                           LicenseClass license_class, std::string_view reason_fragment,
                           bool allow_share_alike = false) {
    const LicenseGate gate = tessera::icons::check_license(set, identifier, allow_share_alike);
    if (gate.accepted) {
        std::cerr << "icons_license: \"" << identifier << "\" should be rejected\n";
        return false;
    }
    if (gate.license.license_class != license_class) {
        std::cerr << "icons_license: \"" << identifier << "\" rejected with the wrong class\n";
        return false;
    }
    if (!contains(gate.reason, reason_fragment)) {
        std::cerr << "icons_license: \"" << identifier << "\" reason is missing \""
                  << reason_fragment << "\": " << gate.reason << "\n";
        return false;
    }
    const std::string message = tessera::icons::license_rejection_message(set, identifier, gate);
    if (!contains(message, set) || !contains(message, identifier) ||
        !contains(message, reason_fragment)) {
        std::cerr << "icons_license: rejection message is incomplete: " << message << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    bool ok = true;

    // Accepted permissive classes (ADR-047 item 8).
    ok = accepts("MIT", LicenseClass::Permissive) && ok;
    ok = accepts("mit", LicenseClass::Permissive) && ok;
    ok = accepts("MIT-0", LicenseClass::Permissive) && ok;
    ok = accepts("ISC", LicenseClass::Permissive) && ok;
    ok = accepts("Apache-2.0", LicenseClass::Permissive) && ok;
    ok = accepts("BSD-2-Clause", LicenseClass::Permissive) && ok;
    ok = accepts("BSD-3-Clause", LicenseClass::Permissive) && ok;
    ok = accepts("0BSD", LicenseClass::Permissive) && ok;
    ok = accepts("CC0-1.0", LicenseClass::Permissive) && ok;
    ok = accepts("OFL-1.1", LicenseClass::Permissive) && ok;
    ok = accepts("Apache License 2.0", LicenseClass::Permissive) && ok;

    // CC-BY needs generated attribution but no flag.
    ok = accepts("CC-BY-4.0", LicenseClass::Attribution) && ok;
    ok = accepts("CC-BY-3.0", LicenseClass::Attribution) && ok;
    ok = expect(tessera::icons::requires_attribution(LicenseClass::Attribution),
                "CC-BY must require attribution") &&
         ok;
    ok = expect(!tessera::icons::requires_attribution(LicenseClass::Permissive),
                "permissive licenses must not require attribution") &&
         ok;

    // Share-alike needs an explicit opt-in.
    ok = rejects("sa", "CC-BY-SA-4.0", LicenseClass::ShareAlike, "share-alike") && ok;
    ok = accepts("CC-BY-SA-4.0", LicenseClass::ShareAlike, true) && ok;
    ok = rejects("sa", "CC-BY-SA-4.0", LicenseClass::ShareAlike, "allow-share-alike") && ok;

    // GPL family is rejected, including unknown variants of the family.
    ok = rejects("gpl", "GPL-3.0", LicenseClass::GplFamily, "GPL-family") && ok;
    ok = rejects("gpl", "GPL-3.0-or-later", LicenseClass::GplFamily, "cannot be bundled") && ok;
    ok = rejects("gpl", "GPL-2.0", LicenseClass::GplFamily, "MIT application") && ok;
    ok = rejects("gpl", "LGPL-2.1", LicenseClass::GplFamily, "GPL-family") && ok;
    ok = rejects("gpl", "AGPL-3.0", LicenseClass::GplFamily, "GPL-family") && ok;

    // Non-commercial is rejected, and NC wins over share-alike when both apply.
    ok = rejects("nc", "CC-BY-NC-4.0", LicenseClass::NonCommercial, "non-commercial") && ok;
    ok =
        rejects("nc", "CC-BY-NC-SA-4.0", LicenseClass::NonCommercial, "non-commercial", true) && ok;

    // Known-but-unlisted and unknown identifiers are rejected with their own reason.
    ok = rejects("mpl", "MPL-2.0", LicenseClass::Unlisted, "owner decision") && ok;
    ok = rejects("bsd4", "BSD-4-Clause", LicenseClass::Unlisted, "advertising clause") && ok;
    ok = rejects("mystery", "", LicenseClass::Unlisted, "records no license") && ok;
    ok = rejects("mystery", "Totally-Made-Up-1.0", LicenseClass::Unlisted,
                 "unknown license identifier") &&
         ok;
    ok = rejects("weird", "BSD-9-Clause", LicenseClass::Unlisted, "variant") && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "icons_license: ok\n";
    return 0;
}
