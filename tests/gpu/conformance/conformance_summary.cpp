// SPDX-License-Identifier: MIT

#include "conformance_summary.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace tessera::tests::gpu::conformance {
namespace {

void append_escaped(std::string& out, std::string_view value) {
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(byte >> 4U) & 0xFU];
                out += kHex[byte & 0xFU];
            } else {
                out += static_cast<char>(byte);
            }
        }
    }
}

void append_quoted(std::string& out, std::string_view value) {
    out += '"';
    append_escaped(out, value);
    out += '"';
}

} // namespace

SummaryTotals totals(const Summary& summary) {
    SummaryTotals counted;
    for (const CheckOutcome& outcome : summary.checks) {
        switch (outcome.status) {
        case Status::Pass:
            ++counted.pass;
            break;
        case Status::Skip:
            ++counted.skip;
            break;
        case Status::Fail:
            ++counted.fail;
            break;
        }
    }
    return counted;
}

bool has_failure(const Summary& summary) {
    return totals(summary).fail != 0;
}

std::string format_summary(const Summary& summary) {
    std::vector<CheckOutcome> ordered = summary.checks;
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const CheckOutcome& left, const CheckOutcome& right) { return left.name < right.name; });

    std::string out;
    out += "{\n";
    out += "  \"backend\": ";
    append_quoted(out, summary.backend);
    out += ",\n";

    if (ordered.empty()) {
        out += "  \"checks\": [],\n";
    } else {
        out += "  \"checks\": [\n";
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const CheckOutcome& outcome = ordered[index];
            out += "    {\n";
            out += "      \"capability\": ";
            append_quoted(out, capability_name(outcome.capability));
            out += ",\n";
            out += "      \"detail\": ";
            append_quoted(out, outcome.detail);
            out += ",\n";
            out += "      \"name\": ";
            append_quoted(out, outcome.name);
            out += ",\n";
            out += "      \"status\": ";
            append_quoted(out, status_name(outcome.status));
            out += "\n";
            out += "    }";
            out += (index + 1U < ordered.size()) ? ",\n" : "\n";
        }
        out += "  ],\n";
    }

    const SummaryTotals counted = totals(summary);
    out += "  \"schema\": 1,\n";
    out += "  \"totals\": {\n";
    out += "    \"fail\": " + std::to_string(counted.fail) + ",\n";
    out += "    \"pass\": " + std::to_string(counted.pass) + ",\n";
    out += "    \"skip\": " + std::to_string(counted.skip) + ",\n";
    out += "    \"total\": " + std::to_string(counted.total()) + "\n";
    out += "  }\n";
    out += "}\n";
    return out;
}

} // namespace tessera::tests::gpu::conformance
