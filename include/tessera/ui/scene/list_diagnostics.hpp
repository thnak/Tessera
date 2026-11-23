// SPDX-License-Identifier: MIT

#pragma once

#include <cstdio>
#include <string_view>

namespace tessera::ui {

// Runtime reports of a generated keyed list (docs/specs/jsx-subset-v1.md section 10.5).
//
// A list whose two live items carry the same `key` has no single node identity to reconcile by.
// The generated list keeps both items and makes the second one's key unique by its position, so
// the rendering stays correct and deterministic; the duplicate itself is reported once per
// duplicate episode. The decision is owner-specified (issue #830): report in debug and headless
// builds, and never fail the build or drop an item.
//
// The seam is a hook for the same reason `TESSERA_BINDING_RAN` is one: a host (the headless
// automation harness, a test) routes the report into its own log, and a production build pays one
// null comparison on the duplicate path. The message is a compile-time literal plus a view into
// the generated key string, so a report allocates nothing (ADR-022: cheap when disabled).
using ListKeyDiagnosticHook = void (*)(const void* component, const char* node_id,
                                       std::string_view key);
inline ListKeyDiagnosticHook g_list_key_diagnostic_hook = nullptr;

inline void list_duplicate_key(const void* component, const char* node_id, std::string_view key) {
    if (g_list_key_diagnostic_hook != nullptr) {
        g_list_key_diagnostic_hook(component, node_id, key);
        return;
    }
#ifndef NDEBUG
    // A debug build reports to stderr so the duplicate is visible without a host wiring anything.
    std::fputs("TESSERA_LIST_DUPLICATE_KEY: ", stderr);
    std::fputs(node_id, stderr);
    std::fputs(" has two live items with key \"", stderr);
    std::fwrite(key.data(), 1, key.size(), stderr);
    std::fputs("\"; the second keeps its position as a suffix\n", stderr);
#else
    (void)component;
    (void)node_id;
    (void)key;
#endif
}

} // namespace tessera::ui

// The generated list calls this on the duplicate path only, so no other path carries a cost.
#define TESSERA_LIST_DUPLICATE_KEY(component, node_id, key)                                        \
    ::tessera::ui::list_duplicate_key((component), (node_id), (key))
