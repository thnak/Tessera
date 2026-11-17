// SPDX-License-Identifier: MIT

// The shared project-file policy (designer-studio-v1.md sections 7 and 10): the one corpus that the
// native `tessera-mcp` server and the Wasm module both judge files with, so the agent and the
// browser cannot disagree about which paths are legal, which files count, and what is too much.

#include "tessera/schema/project_files.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace schema = tessera::schema;
using Kind = schema::ProjectFileKind;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "schema_project_files: " << message << "\n";
        ++failures;
    }
}

void test_paths() {
    for (const std::string_view ok :
         {"tessera.json", "ui/App.tsx", "a/b/c.css", "assets/f o.png", ".hidden"}) {
        expect(!schema::check_project_path(ok).has_value(), "accepts " + std::string(ok));
    }
    struct Bad {
        std::string_view path;
        std::string_view why;
    };
    const Bad bad[] = {
        {"", "empty"},
        {"/etc/passwd", "absolute POSIX"},
        {"C:/Users/x", "drive letter"},
        {"../x", "leading .."},
        {"a/../../x", "climbing .."},
        {"a/./b", "dot segment"},
        {"a//b", "empty segment"},
        {"a/", "trailing slash"},
        {"a\\b", "backslash"},
    };
    for (const Bad& entry : bad) {
        const auto error = schema::check_project_path(entry.path);
        expect(error.has_value(), "refuses " + std::string(entry.why));
        expect(error.has_value() && error->code == schema::kPathEscapeCode,
               "the refusal of " + std::string(entry.why) + " is TESSERA_SEC_PATH_ESCAPE");
    }
    expect(schema::check_project_path(std::string("a\0b", 3)).has_value(), "refuses a NUL byte");
    expect(schema::check_project_path(std::string(1025, 'a')).has_value(),
           "refuses a path over 1024 bytes");
}

void test_classification() {
    const std::vector<std::string> assets = {"fonts/Sans.ttf", "images/"};
    const schema::ProjectFileContext context{"themes/app.json", assets};
    struct Case {
        std::string_view path;
        Kind kind;
    };
    const Case cases[] = {
        {"tessera.json", Kind::Config},
        {"ui/App.tsx", Kind::Source},
        {"ui/Old.jsx", Kind::Source},
        {"ui/app.css", Kind::Style},
        {"themes/app.json", Kind::Theme},
        {"themes/other.json", Kind::Other},
        {"fonts/Sans.ttf", Kind::Asset},
        {"fonts/Other.ttf", Kind::Other},
        {"images/logo.png", Kind::Asset},
        {"images/deep/logo.png", Kind::Asset},
        {"images", Kind::Other},
        {"src/Impl.cpp", Kind::Other},
        {"gen/AppBase.hpp", Kind::Other},
        {"node_modules/dep/index.tsx", Kind::Other},
        {"ui/.cache/App.tsx", Kind::Other},
        {".git/config", Kind::Other},
        {"sub/tessera.json", Kind::Other},
    };
    for (const Case& entry : cases) {
        expect(schema::classify_project_file(entry.path, context) == entry.kind,
               "classifies " + std::string(entry.path) + " as " +
                   std::string(schema::to_string(entry.kind)));
    }
    expect(schema::is_pruned_directory("node_modules") && schema::is_pruned_directory(".git") &&
               !schema::is_pruned_directory("src"),
           "the pruned directories are node_modules and dot directories");
    expect(schema::is_readable(Kind::Source) && !schema::is_readable(Kind::Other),
           "Other is the only unreadable kind");
}

void test_limits() {
    expect(!schema::check_project_limits({}).has_value(), "an empty listing is within the limits");

    std::vector<schema::ProjectFileEntry> many;
    for (std::size_t i = 0; i < schema::kMaxProjectFiles; ++i) {
        many.push_back({"f" + std::to_string(i), 1, Kind::Other});
    }
    expect(!schema::check_project_limits(many).has_value(), "exactly the file limit is allowed");
    many.push_back({"one-more", 1, Kind::Other});
    const auto count = schema::check_project_limits(many);
    expect(count.has_value() && count->code == schema::kFileLimitCode,
           "one file past the limit is TESSERA_SEC_FILE_LIMIT");

    const std::vector<schema::ProjectFileEntry> big = {
        {"ui/Big.tsx", schema::kMaxProjectFileBytes + 1, Kind::Source}};
    const auto too_big = schema::check_project_limits(big);
    expect(too_big.has_value() && too_big->message.find("ui/Big.tsx") != std::string::npos,
           "an over-size accepted file names itself");

    const std::vector<schema::ProjectFileEntry> unaccepted = {
        {"x.bin", schema::kMaxProjectFileBytes * 3, Kind::Other}};
    expect(!schema::check_project_limits(unaccepted).has_value(),
           "an unaccepted file is never counted against the byte limits");

    const std::vector<schema::ProjectFileEntry> edge = {
        {"ui/Edge.tsx", schema::kMaxProjectFileBytes, Kind::Source}};
    expect(!schema::check_project_limits(edge).has_value(),
           "exactly the per-file limit is allowed");

    std::vector<schema::ProjectFileEntry> total;
    for (std::size_t i = 0; i < 21; ++i) {
        total.push_back(
            {"ui/s" + std::to_string(i) + ".tsx", schema::kMaxProjectFileBytes, Kind::Source});
    }
    const auto sum = schema::check_project_limits(total);
    expect(sum.has_value() && sum->message.find("total") != std::string::npos,
           "readable files past the project total are refused");
}

void test_digest() {
    expect(schema::project_file_digest("") == "fnv1a64:cbf29ce484222325",
           "the digest of nothing is the FNV-1a 64 offset basis");
    expect(schema::project_file_digest("a") == "fnv1a64:af63dc4c8601ec8c",
           "the digest of `a` is the published FNV-1a 64 value");
    expect(schema::project_file_digest("abc") != schema::project_file_digest("abd"),
           "different bytes digest differently");
}

} // namespace

int main() {
    test_paths();
    test_classification();
    test_limits();
    test_digest();
    return failures == 0 ? 0 : 1;
}
