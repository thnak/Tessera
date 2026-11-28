// SPDX-License-Identifier: MIT

#pragma once

#include "doc.hpp"
#include "scan.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::format {

// Turns the scanned token stream into a Doc. Layout decisions (where a construct may break, what
// a comment attaches to) are made here; the width decisions are the printer's (doc.cpp).
class Builder {
public:
    Builder(std::string_view source, ScanResult& scan, DocArena& arena)
        : src_(source),
          s_(scan),
          a_(arena) {}

    // The whole file as a statement list. Returns an empty Doc and sets error() on a refusal.
    DocId file();

    [[nodiscard]] const std::string& error() const {
        return error_;
    }
    [[nodiscard]] std::uint32_t error_offset() const {
        return error_offset_;
    }
    [[nodiscard]] bool error_is_comment() const {
        return error_is_comment_;
    }

private:
    // One separate statement of a statement list.
    struct Stmt {
        std::size_t begin = 0;
        std::size_t end = 0;
        bool label = false; // `case x:` or `default:`
    };

    // comments
    DocId comment_doc(std::uint32_t id);
    DocId trailing_docs(std::size_t index, bool space_before_block);
    DocId lead(std::size_t index);
    DocId lead_close(std::size_t closer);
    std::vector<DocId> opener_comments(std::size_t opener, bool lines);
    bool has_opener_comments(std::size_t opener) const;
    [[nodiscard]] bool first_blank(std::size_t index) const;
    void fail_comment(std::uint32_t id, std::string message);

    // tokens
    DocId tok(std::size_t index);
    [[nodiscard]] std::string token_text(std::size_t index) const;
    [[nodiscard]] bool gap_has_space(std::size_t before, std::size_t after) const;

    // code
    DocId run(std::size_t begin, std::size_t end);
    DocId group_doc(std::size_t opener);
    DocId paren_doc(std::size_t opener);
    DocId list_doc(std::size_t opener, bool trailing_comma);
    DocId curly_doc(std::size_t opener);
    DocId block_doc(std::size_t opener);
    DocId statements(std::size_t begin, std::size_t end);
    std::vector<Stmt> split_statements(std::size_t begin, std::size_t end);
    std::vector<std::pair<std::size_t, std::size_t>> split_items(std::size_t begin, std::size_t end,
                                                                 bool semicolons);
    std::string collect_name(std::size_t& cursor, std::size_t& last);
    [[nodiscard]] bool is_block(std::size_t opener) const;
    [[nodiscard]] std::size_t jsx_extent(std::size_t index) const;
    [[nodiscard]] bool starts_statement(std::size_t index) const;
    [[nodiscard]] bool has_top_level_arrow(std::size_t begin, std::size_t end) const;
    DocId join_items(const std::vector<DocId>& items, DocId separator);
    DocId dangling(std::size_t opener, std::size_t closer, std::string_view open_text);

    // jsx
    DocId jsx_wrapped(DocId element);
    std::size_t jsx_element(std::size_t index, DocId& out, bool& forced);
    DocId jsx_container(std::size_t opener);
    DocId jsx_open_tag(std::size_t lt, std::size_t& cursor, std::string& name, bool& self_closing);
    DocId jsx_attribute(std::size_t& cursor);
    bool single_jsx(std::size_t begin, std::size_t end, DocId& out);

    void fail(std::string message, std::uint32_t offset);

    std::string_view src_;
    ScanResult& s_;
    DocArena& a_;
    std::string error_;
    std::uint32_t error_offset_ = 0;
    bool error_is_comment_ = false;
};

} // namespace tessera::compiler::format
