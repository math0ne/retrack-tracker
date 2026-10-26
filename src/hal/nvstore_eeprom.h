// OpenTracker -- INvStore on the K66 FlexRAM emulated EEPROM (spec 02 §9,
// spec 01 §10): the stock config app area, physical byte = offset + 10.
#pragma once

#include "hal_api.h"

namespace ot {
namespace hal {

INvStore& nvStore();

}  // namespace hal
}  // namespace ot
