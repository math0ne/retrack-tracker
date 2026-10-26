// OpenTracker simulator -- IMidiPort. With ALSA (OT_SIM_ALSA) it creates an
// ALSA sequencer client "OpenTracker" with two virtual duplex ports,
// "OpenTracker DIN" and "OpenTracker USB"; connect them with aconnect or a DAW.
// Without ALSA (or if the sequencer cannot be opened) it is a null port.
#pragma once

#include <cstdint>

#include "hal_api.h"

namespace sim {

class HostMidi final : public ot::IMidiPort {
public:
    HostMidi();
    ~HostMidi() override;
    bool open();   // false -> null port
    bool active() const { return impl_ != nullptr; }
    bool send(ot::MidiPortId port, const uint8_t* data, uint32_t len) override;
    uint32_t receive(ot::MidiPortId port, uint8_t* dst, uint32_t cap) override;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace sim
