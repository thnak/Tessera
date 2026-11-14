// SPDX-License-Identifier: MIT
//
// License gate for Iconify sets (ADR-047 item 8, docs/design/component-library.md section 3.1).
//
// The classes come straight from the ADR: MIT, ISC, Apache-2.0, BSD, CC0 and OFL-1.1 pass; CC-BY
// passes with generated attribution; share-alike needs an explicit opt-in; GPL-family,
// non-commercial and unknown licenses are rejected. Sets that mix licenses carry the strictest one
// in their metadata, so classifying the set's declared license is enough to keep one icon from
// pulling an incompatible license into an app.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::icons {

enum class LicenseClass : std::uint8_t {
    Permissive,    // MIT, ISC, Apache-2.0, BSD-2/3-Clause, 0BSD, CC0-1.0, OFL-1.1
    Attribution,   // CC-BY: accepted, but the importer must generate attribution
    ShareAlike,    // CC-BY-SA: accepted only with an explicit opt-in
    GplFamily,     // GPL/LGPL/AGPL: rejected, incompatible with an MIT application
    NonCommercial, // NC variants: rejected, cannot ship
    Unlisted,      // everything else, including a missing or ambiguous license
};

struct LicenseInfo {
    // Normalized identifier: trimmed, lowercased, spaces and underscores folded to '-'.
    std::string id;
    LicenseClass license_class = LicenseClass::Unlisted;
    // Why an otherwise-known license is not on the allowlist (empty for allowlisted identifiers).
    std::string note;
};

// Classifies an SPDX identifier or license title from set metadata.
[[nodiscard]] LicenseInfo classify_license(std::string_view raw);

struct LicenseGate {
    bool accepted = false;
    LicenseInfo license;
    // Human-readable reason; set exactly when `accepted` is false.
    std::string reason;
};

// Applies the ADR-047 policy to one set's declared license.
[[nodiscard]] LicenseGate check_license(std::string_view set, std::string_view raw,
                                        bool allow_share_alike);

// True for the class the importer must write attribution for (CC-BY).
[[nodiscard]] bool requires_attribution(LicenseClass license_class) noexcept;

// One diagnostic line naming the set, the license exactly as recorded and the reason. Stable text:
// the unit tests assert all three parts are present.
[[nodiscard]] std::string license_rejection_message(std::string_view set, std::string_view raw,
                                                    const LicenseGate& gate);

} // namespace tessera::icons
