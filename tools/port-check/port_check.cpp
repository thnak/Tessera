// SPDX-License-Identifier: MIT
//
// The manifest reader and the two renderings (issue #546).
//
// **Why a hand-rolled reader and not a JSON library.** The repository has no JSON dependency in
// `tools/`, and adding one for three string arrays would put a parser in the build for a tool that
// reads `dependencies`, `devDependencies` and `peerDependencies` and nothing else. What is needed
// is narrow enough to state exactly: find those three keys, and inside each take the object's
// string keys. Anything outside them is ignored, which is what makes the reader robust to the parts
// of a real `package.json` (scripts, workspaces, `"exports"`) that this tool has no opinion about.
//
// The failure it must not have is a silent one: a manifest that is not an object, or a dependency
// section that is not an object, is an error with a position rather than an empty report that looks
// like "nothing to port".

#include "port_check.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::port_check {
namespace {

// Where a value starts in `text`, or `npos`.
constexpr std::size_t kNpos = std::string_view::npos;

void skip_whitespace(std::string_view text, std::size_t& at) {
    while (at < text.size() &&
           (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
        ++at;
    }
}

// The end of a JSON string that starts at `at` (which must be the opening quote), or `npos`.
// Handles the escapes a package name or a version can contain; a control character inside a string
// ends the scan, because a manifest with one is not usable and saying so is better than guessing.
std::size_t string_end(std::string_view text, std::size_t at) {
    if (at >= text.size() || text[at] != '"') {
        return kNpos;
    }
    ++at;
    while (at < text.size()) {
        const char c = text[at];
        if (c == '\\') {
            at += 2;
            continue;
        }
        if (c == '"') {
            return at + 1;
        }
        if (static_cast<unsigned char>(c) < 0x20U) {
            return kNpos;
        }
        ++at;
    }
    return kNpos;
}

// Reads the string starting at `at` into `out`, honouring the two escapes that appear in practice
// (`\"` and `\\`). The manifest's other escapes are legal but irrelevant to a package name, and
// leaving them is better than a half-implemented decoder: a name that needed one would not be a
// package name.
std::string read_string(std::string_view text, std::size_t at, std::size_t end) {
    std::string out;
    out.reserve(end - at);
    ++at; // the opening quote
    while (at + 1 < end) {
        if (text[at] == '\\' && at + 1 < end) {
            out.push_back(text[at + 1]);
            at += 2;
            continue;
        }
        out.push_back(text[at]);
        ++at;
    }
    return out;
}

// Skips one JSON value of any kind, so an object this tool does not care about can be stepped over.
// Returns the position just past it, or `npos` on a shape it cannot follow.
std::size_t skip_value(std::string_view text, std::size_t at);

std::size_t skip_object_or_array(std::string_view text, std::size_t at) {
    const char open = text[at];
    const char close = open == '{' ? '}' : ']';
    ++at;
    while (at < text.size()) {
        skip_whitespace(text, at);
        if (at < text.size() && text[at] == close) {
            return at + 1;
        }
        if (at < text.size() && text[at] == ',') {
            ++at;
            continue;
        }
        if (at < text.size() && open == '{') {
            const std::size_t key_end = string_end(text, at);
            if (key_end == kNpos) {
                return kNpos;
            }
            at = key_end;
            skip_whitespace(text, at);
            if (at >= text.size() || text[at] != ':') {
                return kNpos;
            }
            at += 1;
            at = skip_value(text, at);
            if (at == kNpos) {
                return kNpos;
            }
            continue;
        }
        at = skip_value(text, at);
        if (at == kNpos) {
            return kNpos;
        }
    }
    return kNpos;
}

std::size_t skip_value(std::string_view text, std::size_t at) {
    skip_whitespace(text, at);
    if (at >= text.size()) {
        return kNpos;
    }
    const char c = text[at];
    if (c == '"') {
        return string_end(text, at);
    }
    if (c == '{' || c == '[') {
        return skip_object_or_array(text, at);
    }
    // A number, `true`, `false` or `null`: run to the delimiter.
    while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ']' &&
           text[at] != ' ' && text[at] != '\n' && text[at] != '\r' && text[at] != '\t') {
        ++at;
    }
    return at;
}

// Calls `visit(name)` for each key of the object value that starts at `at` (the opening brace).
// Returns false when the text is not the object shape this tool expects.
template<typename Visit> bool for_each_key(std::string_view text, std::size_t at, Visit&& visit) {
    skip_whitespace(text, at);
    if (at >= text.size() || text[at] != '{') {
        return false;
    }
    ++at;
    while (at < text.size()) {
        skip_whitespace(text, at);
        if (at < text.size() && text[at] == '}') {
            return true;
        }
        if (at < text.size() && text[at] == ',') {
            ++at;
            continue;
        }
        const std::size_t key_end = string_end(text, at);
        if (key_end == kNpos) {
            return false;
        }
        const std::string key = read_string(text, at, key_end);
        at = key_end;
        skip_whitespace(text, at);
        if (at >= text.size() || text[at] != ':') {
            return false;
        }
        ++at;
        skip_whitespace(text, at);
        visit(key, at);
        at = skip_value(text, at);
        if (at == kNpos) {
            return false;
        }
    }
    return false;
}

// The value position of `wanted` in the top-level object, or `kNpos`.
std::size_t find_member(std::string_view text, std::string_view wanted) {
    std::size_t found = kNpos;
    (void)for_each_key(text, 0, [&](const std::string& key, std::size_t value_at) {
        if (key == wanted) {
            found = value_at;
        }
    });
    return found;
}

// Collects the dependency names of one section into `out`.
bool read_section(std::string_view text, std::string_view section, const char* section_label,
                  std::vector<DependencyReport>& out) {
    const std::size_t at = find_member(text, section);
    if (at == kNpos) {
        return true; // a project with no devDependencies is ordinary, not an error
    }
    std::vector<std::string> names;
    if (!for_each_key(text, at,
                      [&](const std::string& key, std::size_t) { names.push_back(key); })) {
        return false;
    }
    for (const std::string& name : names) {
        const ClassRow* row = find_row(name);
        DependencyReport& entry = out.emplace_back();
        entry.name = name;
        entry.section = section_label;
        entry.is_dev = section == "devDependencies";
        entry.port_class = row->port_class;
        entry.replacement = std::string(row->replacement);
        entry.source = std::string(row->source);
    }
    return true;
}

void append_escaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
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
            if (static_cast<unsigned char>(c) < 0x20U) {
                out += '?';
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

std::size_t class_index(PortClass value) {
    switch (value) {
    case PortClass::SourceCompatible:
        return 0;
    case PortClass::Shim:
        return 1;
    case PortClass::BuildTime:
        return 2;
    case PortClass::NotPorted:
        return 3;
    }
    return 3;
}

} // namespace

PortReport check_manifest(std::string_view project_path, std::string_view package_json_text) {
    PortReport report;
    report.project_path = std::string(project_path);

    // The file has to be an object before anything else is worth asking.
    std::size_t first = 0;
    skip_whitespace(package_json_text, first);
    if (first >= package_json_text.size() || package_json_text[first] != '{') {
        report.error = "package.json is not a JSON object";
        return report;
    }

    // Both runtime and development dependencies are classified: a build tool in devDependencies is
    // still a dependency the port has to answer for, and ADR-058's classes are about the API, not
    // about which section declared it.
    if (!read_section(package_json_text, "dependencies", "dependencies", report.dependencies)) {
        report.error = "the \"dependencies\" section is not an object of names";
        return report;
    }
    if (!read_section(package_json_text, "devDependencies", "devDependencies",
                      report.dependencies)) {
        report.error = "the \"devDependencies\" section is not an object of names";
        return report;
    }
    if (!read_section(package_json_text, "peerDependencies", "peerDependencies",
                      report.dependencies)) {
        report.error = "the \"peerDependencies\" section is not an object of names";
        return report;
    }

    // Sorted by name so two runs of the same project produce the same report: the acceptance asks
    // for golden output, which a map's iteration order would not give.
    std::sort(report.dependencies.begin(), report.dependencies.end(),
              [](const DependencyReport& lhs, const DependencyReport& rhs) {
                  return lhs.name < rhs.name;
              });

    for (const DependencyReport& dependency : report.dependencies) {
        ++report.per_class[class_index(dependency.port_class)];
        if (dependency.port_class == PortClass::NotPorted) {
            report.blockers.push_back(dependency.name);
        }
    }
    return report;
}

std::string render_text(const PortReport& report) {
    std::string out;
    out += "port-check: ";
    out += report.project_path;
    out += '\n';
    if (!report.error.empty()) {
        out += "error: ";
        out += report.error;
        out += '\n';
        return out;
    }
    if (report.dependencies.empty()) {
        out += "no dependencies: nothing to classify\n";
        return out;
    }
    std::size_t width = 0;
    for (const DependencyReport& dependency : report.dependencies) {
        width = std::max(width, dependency.name.size());
    }
    for (const DependencyReport& dependency : report.dependencies) {
        out += "  ";
        out += class_letter(dependency.port_class);
        out += "  ";
        out += dependency.name;
        out += std::string(width - dependency.name.size() + 2, ' ');
        out += dependency.is_dev ? "(dev) " : "      ";
        out += dependency.replacement.empty() ? "ready" : dependency.replacement;
        out += '\n';
    }
    out += "  A ";
    out += std::to_string(report.per_class[0]);
    out += "  B ";
    out += std::to_string(report.per_class[1]);
    out += "  C ";
    out += std::to_string(report.per_class[2]);
    out += "  D ";
    out += std::to_string(report.per_class[3]);
    out += '\n';
    if (report.blockers.empty()) {
        out += "no blockers\n";
    } else {
        out += "blockers: ";
        for (std::size_t index = 0; index < report.blockers.size(); ++index) {
            if (index != 0) {
                out += ", ";
            }
            out += report.blockers[index];
        }
        out += '\n';
    }
    return out;
}

std::string render_json(const PortReport& report) {
    std::string out = "{\"portCheckVersion\":1,\"project\":";
    append_escaped(out, report.project_path);
    if (!report.error.empty()) {
        out += ",\"error\":";
        append_escaped(out, report.error);
        out += "}\n";
        return out;
    }
    out += ",\"dependencies\":[";
    for (std::size_t index = 0; index < report.dependencies.size(); ++index) {
        if (index != 0) {
            out += ',';
        }
        const DependencyReport& dependency = report.dependencies[index];
        out += "{\"name\":";
        append_escaped(out, dependency.name);
        out += ",\"section\":";
        append_escaped(out, dependency.section);
        out += ",\"class\":";
        out.push_back('"');
        out.push_back(class_letter(dependency.port_class));
        out.push_back('"');
        out += ",\"replacement\":";
        append_escaped(out, dependency.replacement);
        out += ",\"source\":";
        append_escaped(out, dependency.source);
        out.push_back('}');
    }
    out += "],\"counts\":{\"A\":";
    out += std::to_string(report.per_class[0]);
    out += ",\"B\":";
    out += std::to_string(report.per_class[1]);
    out += ",\"C\":";
    out += std::to_string(report.per_class[2]);
    out += ",\"D\":";
    out += std::to_string(report.per_class[3]);
    out += "},\"blockers\":[";
    for (std::size_t index = 0; index < report.blockers.size(); ++index) {
        if (index != 0) {
            out += ',';
        }
        append_escaped(out, report.blockers[index]);
    }
    out += "]}\n";
    return out;
}

int exit_code(const PortReport& report) {
    if (!report.error.empty()) {
        return 2;
    }
    return report.blockers.empty() ? 0 : 1;
}

} // namespace tessera::port_check
