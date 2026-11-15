// SPDX-License-Identifier: MIT
//
// The service interfaces the AF5 fixture component resolves (ADR-050 section 3, issue #177). In a
// real project these come from `ui/services.json` (ADR-050 section 3.1 item 1); here they are the
// hand-written C++ the manifest will eventually point at, which is exactly what the generated base
// needs: an abstract contract with a compile-time `kServiceId`, and a concrete implementation that
// the composition root provides under it.

#pragma once

#include "tessera/appfw/service_id.hpp"

#include <cstdint>
#include <string>

namespace demo {

// A navigation contract: the ADR-050 section 3.1 example.
class Navigator {
public:
    static constexpr tessera::appfw::ServiceId kServiceId = tessera::appfw::service_id("demo.nav");
    virtual ~Navigator() = default;
    virtual void push(std::string route) = 0;
    virtual std::uint64_t pushes() const = 0;
    virtual const std::string& last_route() const = 0;
};

class MailStore {
public:
    static constexpr tessera::appfw::ServiceId kServiceId =
        tessera::appfw::service_id("demo.mail-store");
    virtual ~MailStore() = default;
    virtual std::uint64_t unread() const = 0;
};

// A concrete implementation of each contract, what the composition root provides.
class NavigatorImpl final : public Navigator {
public:
    void push(std::string route) override {
        last_route_ = std::move(route);
        ++pushes_;
    }
    [[nodiscard]] std::uint64_t pushes() const override {
        return pushes_;
    }
    [[nodiscard]] const std::string& last_route() const override {
        return last_route_;
    }

private:
    std::uint64_t pushes_ = 0;
    std::string last_route_;
};

class MailStoreImpl final : public MailStore {
public:
    explicit MailStoreImpl(std::uint64_t unread)
        : unread_(unread) {}
    [[nodiscard]] std::uint64_t unread() const override {
        return unread_;
    }

private:
    std::uint64_t unread_ = 0;
};

// A type with no `kServiceId`: the `TESSERA_CG_SERVICE_NOT_INTERFACE` fixture. Using it as a
// `useService` type is what that code rejects.
struct NotAService {
    int value = 0;
};

// A registered service interface the fixture component does NOT declare: the
// `TESSERA_CG_SERVICE_UNDECLARED` fixture. It is a perfectly valid service; the component simply
// never asked for it, which is the mistake the code catches.
class Settings {
public:
    static constexpr tessera::appfw::ServiceId kServiceId =
        tessera::appfw::service_id("demo.settings");
    virtual ~Settings() = default;
    virtual int theme() const = 0;
};

} // namespace demo
