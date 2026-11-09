// SPDX-License-Identifier: MIT

#include "form_async.hpp"

#include <utility>

namespace tessera::ui::forms::detail {

runtime::Task<void> run_async_bridge(runtime::TaskContext context, AsyncValidateFn validator,
                                     FormValue value, std::function<void(bool)> done) {
    bool valid = false;
    try {
        valid = co_await validator(context, value);
    } catch (...) {
        // Rule evaluation is value-based: an async validator throw is reported as invalid rather
        // than escaping the form. (Custom async validators should return false for "invalid".)
        valid = false;
    }
    if (done) {
        done(valid);
    }
}

} // namespace tessera::ui::forms::detail
