// OpenTracker -- I2S receive liveness check (spec 22 §7 row 21, §8 item 2).
// Platform independent so the host tests drive it with a fake receiver
// (tests/rx_watch_test.cpp); the device side is audio_sgtl5000.cpp.
//
// The line input hands the app the most recently completed RX half. If the
// receiver halts (RCSR FEF, bit 18, or the RX DMA no longer completing
// blocks), that half never changes again, and the recorder would meter and
// record a frozen block. use() is called once per audio block (TX callback
// context) with the RX block counter and the FEF bit: as soon as the counter
// has stood still for kStallBlocks blocks, or FEF is set, the input is reported
// stale (no block handed out) until the main loop has restarted the receiver
// and called recovered().
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {

class RxWatch {
public:
    // TX and RX run on the same frame clock, so every TX block normally sees
    // one new RX block; a few blocks of slack absorb ISR jitter.
    static constexpr uint32_t kStallBlocks = 4;

    // Audio callback context. true = the latest RX block is live and may be used.
    bool use(uint32_t rxBlocks, bool fifoError) {
        if (rxBlocks != lastRx_) {
            lastRx_ = rxBlocks;
            idle_ = 0;
        } else if (idle_ < kStallBlocks) {
            idle_++;
        }
        if (!stalled_ && (fifoError || idle_ >= kStallBlocks)) {
            stalled_ = true;
            fifoError_ = fifoError;
        }
        if (stalled_) staleBlocks_++;
        return !stalled_;
    }

    // Main loop: a restart of the receiver is due.
    bool needsRecovery() const { return stalled_; }
    bool stallWasFifoError() const { return fifoError_; }
    // Main loop, after the receiver was restarted (with the audio IRQs masked).
    void recovered(uint32_t rxBlocks) {
        lastRx_ = rxBlocks;
        idle_ = 0;
        stalled_ = false;
        fifoError_ = false;
        recoveries_++;
    }

    uint32_t recoveries() const { return recoveries_; }
    uint32_t staleBlocks() const { return staleBlocks_; }

    // The block for the app (HalRefs::lineIn): the completed RX half `half`
    // (0/1, -1 = none yet) of `ring` (two halves of `halfSamples`), or nullptr
    // while the receiver is stale -- never the same frozen block again.
    const int16_t* pick(uint32_t rxBlocks, bool fifoError, int half, const int16_t* ring,
                        uint32_t halfSamples) {
        if (!use(rxBlocks, fifoError) || half < 0) return nullptr;
        return ring + uint32_t(half) * halfSamples;
    }

private:
    volatile uint32_t lastRx_ = 0;
    volatile uint32_t idle_ = 0;
    volatile bool stalled_ = false;
    volatile bool fifoError_ = false;
    volatile uint32_t recoveries_ = 0;
    volatile uint32_t staleBlocks_ = 0;
};

}  // namespace hal
}  // namespace ot
