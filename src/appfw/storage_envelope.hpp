// SPDX-License-Identifier: MIT
//
// The stored-entry envelope: a small header in front of the payload, so a torn, truncated or
// hand-edited file is detected instead of being returned as data (docs/design/services.md section
// 4: "corruption recovery"; issue #179 acceptance: "a corrupted file falls back with a
// diagnostic").
//
// Layout, little-endian, 24 bytes:
//
//   0  4  magic "TSR1"
//   4  2  format version
//   6  2  kind (0 = opaque document, 1 = settings list)
//   8  8  payload length
//  16  8  FNV-1a 64 of the payload
//
// The checksum is a corruption detector, not a security measure: an attacker who can write the file
// can forge it. Integrity against tampering is the restore snapshot's encryption (state-restoration
// v1 section 3), not this header.
//
// The length field is what makes a truncated file detectable *before* the payload is parsed, and
// the checksum is what catches a file that was replaced with garbage of a plausible size. Both are
// cheap, which matters because this runs on every read.

#pragma once

#include "tessera/appfw/storage.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw::detail {

inline constexpr std::size_t kEnvelopeHeaderBytes = 24;
inline constexpr std::string_view kEnvelopeMagic = "TSR1";

enum class EnvelopeKind : std::uint16_t {
    Document = 0,
    Settings = 1,
};

// FNV-1a 64, the same function `service_id` uses; one hash in the codebase, not two.
[[nodiscard]] std::uint64_t envelope_checksum(std::string_view payload) noexcept;

// Produces `header || payload`. Returns `TooLarge` when the payload exceeds `max_payload`.
[[nodiscard]] std::expected<std::string, StorageError> encode_envelope(EnvelopeKind kind,
                                                                       std::uint32_t version,
                                                                       std::string_view payload,
                                                                       std::size_t max_payload);

// One decoded entry.
struct DecodedEnvelope {
    EnvelopeKind kind = EnvelopeKind::Document;
    std::uint32_t version = 0;
    std::string payload;
};

// Every failure mode is `Corrupted` with the step naming which field disagreed, so a diagnostic
// tells a truncated file from a bad checksum from a future format version.
[[nodiscard]] std::expected<DecodedEnvelope, StorageError>
decode_envelope(std::string_view bytes, EnvelopeKind expected_kind, std::uint32_t max_version,
                std::size_t max_payload);

} // namespace tessera::appfw::detail
