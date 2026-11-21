// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// `hotkeys` (ADR-064 item 7, issue #627): declare keyboard shortcuts for a scope.
//
// The engine owns the mechanism: `KeyBindingRegistry` is a bounded, allocation-free table of chords
// with a documented matching rule (exact modifiers first, broad bindings after, at most one fires
// per event), and `InputRouter` already dispatches it after the focused node's handler and only
// when the key event was not consumed. This behavior is the hook's half: it registers the chords
// the component declares, removes them on detach, and reports whether a binding is currently live.
//
// **Why the behavior does not own the registry.** The registry is the window's, not a component's:
// `InputRouter::accelerators()` is the one table the input path consults. A behavior that kept its
// own table would never be consulted. So the behavior registers into the router's registry and
// holds the ids it got, which is what makes detach exact.
//
// **Why a chord is a string.** The manifest declares a closed token list, so the analyzer can check
// the spelling at compile time, and the behavior parses it once at attach. A chord is
// `[modifier+]*key`, where a modifier is `ctrl`, `alt`, `shift` or `meta` and the key is a name
// from `key_code.hpp` (`s`, `f1`, `escape`, `arrowup`, ...).
namespace tessera::ui::behavior {

class Hotkeys final : public Behavior {
public:
    // The manifest's output order: `registered`.
    static constexpr std::size_t kOutputCount = 1;
    // The most chords one hook may declare. Bounded so the behavior allocates nothing: the registry
    // itself caps at 64 bindings for the whole window, and one component declaring more than this
    // is a design error the analyzer reports.
    static constexpr std::size_t kMaxChords = 8;

    Hotkeys() = default;
    ~Hotkeys() override;

    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

    // How many chords this behavior currently has registered. Exposed so a test can assert the
    // attach/detach lifecycle without reaching into private state.
    [[nodiscard]] std::size_t registered() const {
        return count_;
    }

    // Parses one chord (`ctrl+s`, `shift+f1`, `escape`). Returns false for an unknown modifier or
    // key name. Public so the parser is testable without a scene.
    struct Chord {
        std::uint32_t key = 0;
        std::uint32_t modifiers = 0;
    };
    [[nodiscard]] static bool parse_chord(std::string_view text, Chord& out);

private:
    // Registers every chord in `args` and recomputes the output. Called from `attach` and `update`.
    void apply(const ArgsView& args);
    // Removes every registration this behavior holds.
    void release();
    // Whether the scene's focused node is an editable text field, which suppresses a chord unless
    // `allowInTextInput` is set.
    [[nodiscard]] bool text_input_focused() const;

    InputRouter* router_ = nullptr;
    std::array<std::uint32_t, kMaxChords> ids_{};
    std::size_t count_ = 0;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
