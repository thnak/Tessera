// SPDX-License-Identifier: MIT

#include "tessera/ui/boundary/guard.hpp"

#include <exception>
#include <new>

namespace tessera::ui::boundary {

Error capture_current_error() {
    // The catch order classifies without RTTI: each marker type is matched statically, and
    // std::bad_alloc is matched before std::exception so OOM keeps its Fatal class.
    try {
        throw;
    } catch (const CancellationSignal& error) {
        return make_error(error);
    } catch (const PlatformFailure& error) {
        return make_error(error);
    } catch (const std::bad_alloc& error) {
        return make_error(error);
    } catch (const std::exception& error) {
        return make_error(error);
    } catch (...) {
        return make_error();
    }
}

} // namespace tessera::ui::boundary
