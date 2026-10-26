// Static initialization for stmlib compatibility layer.
// Copyright 2012-2014 Emilie Gillet (MIT License for original stmlib code)

#include "looper/clouds/stmlib_compat.h"

namespace stmlib {

uint32_t Random::rng_state_ = 0x12345678;

}  // namespace stmlib
