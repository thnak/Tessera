// SPDX-License-Identifier: MIT

// The report text of the corpus measurement. Kept apart from the measurement itself so the two
// tables and the per-file list can be read and edited without touching the stages.

#include "measure.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::corpus {
namespace {

// `part / whole` as a percentage with one decimal, computed in integers so no locale or
// floating-point formatting can change the bytes.
std::string percentage(std::size_t part, std::size_t whole) {
    if (whole == 0) {
        return "0.0%";
    }
    const std::size_t tenths = (part * 1000 + whole / 2) / whole;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "%";
}

void add(std::map<std::string, CodeFrequency>& table, const std::string& code) {
    CodeFrequency& entry = table[code];
    entry.code = code;
    ++entry.occurrences;
}

// Occurrences descending, then code ascending: the order the ADR's order of work is read in.
std::vector<CodeFrequency> by_occurrences(std::map<std::string, CodeFrequency> table) {
    std::vector<CodeFrequency> out;
    out.reserve(table.size());
    for (const auto& [code, entry] : table) {
        out.push_back(entry);
    }
    std::sort(out.begin(), out.end(), [](const CodeFrequency& left, const CodeFrequency& right) {
        if (left.occurrences != right.occurrences) {
            return left.occurrences > right.occurrences;
        }
        return left.code < right.code;
    });
    return out;
}

} // namespace

Report summarize(std::vector<FileMeasurement> files) {
    Report report;
    std::sort(files.begin(), files.end(),
              [](const FileMeasurement& left, const FileMeasurement& right) {
                  return left.path < right.path;
              });
    report.files = std::move(files);

    std::map<std::string, CodeFrequency> occurrences;
    std::map<std::string, CodeFrequency> firsts;
    std::map<std::string, CodeFrequency> pipeline_files;
    for (const FileMeasurement& file : report.files) {
        if (file.accepted) {
            ++report.accepted;
            if (file.components > 0) {
                ++report.accepted_components;
            }
        }
        if (file.pipeline_accepted) {
            ++report.pipeline_accepted;
        } else if (!file.pipeline_code.empty()) {
            add(pipeline_files, file.pipeline_code);
        }
        report.warning_count += file.warnings.size();
        if (!file.errors.empty()) {
            add(firsts, file.errors.front());
        }
        std::vector<std::string> seen;
        for (const std::string& code : file.errors) {
            add(occurrences, code);
            if (std::find(seen.begin(), seen.end(), code) == seen.end()) {
                seen.push_back(code);
                ++occurrences[code].files;
            }
        }
    }
    report.rejections = by_occurrences(std::move(occurrences));
    report.first_rejections = by_occurrences(std::move(firsts));
    report.pipeline_rejections = by_occurrences(std::move(pipeline_files));
    return report;
}

std::string format_report(const Report& report) {
    std::string out;
    out += "corpus: " + std::to_string(report.total()) + " file(s)\n";
    out += "front end accepted: " + std::to_string(report.accepted) + " (" +
           percentage(report.accepted, report.total()) + "), " +
           std::to_string(report.total() - report.accepted) + " rejected\n";
    out += "front end accepted with a component declaration: " +
           std::to_string(report.accepted_components) + " (" +
           percentage(report.accepted_components, report.total()) + ")\n";
    out += "pipeline accepted: " + std::to_string(report.pipeline_accepted) + " (" +
           percentage(report.pipeline_accepted, report.total()) + ")\n";
    out += "warnings: " + std::to_string(report.warning_count) + "\n";

    out += "\nfront-end rejections (code, occurrences, files):\n";
    if (report.rejections.empty()) {
        out += "  (none)\n";
    }
    for (const CodeFrequency& entry : report.rejections) {
        out += "  " + entry.code + " " + std::to_string(entry.occurrences) + " " +
               std::to_string(entry.files) + "\n";
    }

    out += "\nfront-end first rejections (code, files):\n";
    if (report.first_rejections.empty()) {
        out += "  (none)\n";
    }
    for (const CodeFrequency& entry : report.first_rejections) {
        out += "  " + entry.code + " " + std::to_string(entry.occurrences) + "\n";
    }

    out += "\npipeline rejections (first blocking code, files):\n";
    if (report.pipeline_rejections.empty()) {
        out += "  (none)\n";
    }
    for (const CodeFrequency& entry : report.pipeline_rejections) {
        out += "  " + entry.code + " " + std::to_string(entry.occurrences) + "\n";
    }

    out += "\nfiles:\n";
    for (const FileMeasurement& file : report.files) {
        out += file.accepted ? "  accepted " : "  rejected ";
        out += file.path;
        out += " components=" + std::to_string(file.components);
        if (!file.accepted) {
            out += ":";
            for (const std::string& code : file.errors) {
                out += " " + code;
            }
        }
        out += file.pipeline_accepted ? " pipeline=ok" : " pipeline=" + file.pipeline_code;
        out += "\n";
    }
    return out;
}

} // namespace tessera::compiler::corpus
