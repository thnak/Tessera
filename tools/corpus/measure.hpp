// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Front-end corpus measurement (spike #660, ADR-066 "Evidence required before Accepted").
//
// The question this answers is "what does the JSX/TSX front end accept today, and why does it
// reject the rest", so the order of work of ADR-066 item 6 is chosen by data instead of by
// intuition. The unit of measurement is one source file.
//
// Two stages are measured, because they answer two different questions and a port needs both:
//
//   * `front_end` -- parse plus the Tier-1 rules. Every error diagnostic is counted, once per
//     occurrence and once per file, so a form that appears twice in one file does not look like two
//     files. This is the table that ranks the React forms.
//   * `pipeline` -- the whole single-file path the CLI and the preview run (parse, analyse, lower,
//     validate), reporting the FIRST blocking diagnostic, which is the convention
//     `docs/specs/compiler-diagnostics-v1.md` section 7 defines for a caller that shows one error.
//     This is the honest "does the port compile" number: a file the front end accepts can still be
//     rejected by lowering (an unresolved import, a missing stylesheet, a missing id).
//
// Both are deterministic: a pure function of `text` and `path`, with no clock, locale, environment
// or filesystem read.
namespace tessera::compiler::corpus {

struct FileMeasurement {
    std::string path; // as the caller spells it; the report sorts by it
    bool accepted = false;
    std::size_t components = 0;        // component declarations the front end recognized
    std::vector<std::string> errors;   // diagnostic codes, in report order
    std::vector<std::string> warnings; // codes of Severity::Warning, in report order
    // The pipeline stage: the first blocking code, or empty when the file compiles.
    std::string pipeline_code;
    bool pipeline_accepted = false;
};

struct CodeFrequency {
    std::string code;
    std::size_t occurrences = 0;
    std::size_t files = 0;
};

struct Report {
    std::vector<FileMeasurement> files;    // sorted by path
    std::vector<CodeFrequency> rejections; // occurrences descending, then code ascending
    // The first error of each rejected file. This is the table the order of work is read from: a
    // file blocked by an earlier form cannot show what else it would need, so "files unblocked by
    // fixing this one code" is the first-rejection count, not the occurrence count.
    std::vector<CodeFrequency> first_rejections;    // files descending, then code ascending
    std::vector<CodeFrequency> pipeline_rejections; // files descending, then code ascending
    std::size_t accepted = 0;
    std::size_t accepted_components = 0;
    std::size_t pipeline_accepted = 0;
    std::size_t warning_count = 0;

    [[nodiscard]] std::size_t total() const {
        return files.size();
    }
};

// Parses and analyses one source. Deterministic: a pure function of `text` and `path`.
[[nodiscard]] FileMeasurement measure_source(std::string path, std::string_view text);

// Adds the pipeline stage's verdict to an existing measurement. Separate from `measure_source` so a
// caller that only wants the front-end table does not pay for lowering.
void measure_pipeline(FileMeasurement& measurement, std::string_view text);

// Measures every `.tsx` file under `root`, recursively, in sorted path order. Each file is named
// `<label>/<path relative to root>`, so the report does not depend on where the checkout lives and
// two roots cannot collide. `label` is the caller's choice: the CLI passes the root as it was
// written, the golden test passes the corpus's own name. Returns an empty vector when `root` is not
// a directory; a file that cannot be read is reported through `error` and skipped.
[[nodiscard]] std::vector<FileMeasurement>
measure_directory(const std::string& root, const std::string& label, std::string& error);

// Aggregates measurements into the report. Sorts `files` by path and each frequency table by its
// documented key, so two runs over the same corpus are byte-identical.
[[nodiscard]] Report summarize(std::vector<FileMeasurement> files);

// The human-readable report the golden holds. Ends with a newline.
[[nodiscard]] std::string format_report(const Report& report);

} // namespace tessera::compiler::corpus
