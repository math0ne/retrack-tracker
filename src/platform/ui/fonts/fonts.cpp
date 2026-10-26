// The generated RAM_G font image (fonts.h). Defined in exactly one unit so the
// 126 KB table exists once in flash.
#include "ui/fonts/fonts.h"

namespace ot {
namespace fontdata {

alignas(4) const uint8_t kBlob[kBlobSize] = {
#include "ui/fonts/font_data.inc"
};

static_assert(kRamGBase % 4 == 0, "font image must be 4-byte aligned in RAM_G");

}  // namespace fontdata
}  // namespace ot
