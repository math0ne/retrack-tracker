// OpenTracker -- first-boot hardware smoke test (call after hal::boardInit()).
#pragma once

namespace ot {
namespace hal {

[[noreturn]] void runSelftest();

}  // namespace hal
}  // namespace ot
