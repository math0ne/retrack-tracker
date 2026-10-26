#include "host_midi.h"

#include <cstdio>

#ifdef OT_SIM_ALSA
#include <alsa/asoundlib.h>
#endif

namespace sim {

#ifdef OT_SIM_ALSA
struct HostMidi::Impl {
    snd_seq_t* seq = nullptr;
    int port[2] = {-1, -1};
    snd_midi_event_t* enc[2] = {};
    snd_midi_event_t* dec = nullptr;
    static constexpr uint32_t kRx = 4096;
    uint8_t rx[2][kRx] = {};
    uint32_t rxHead[2] = {}, rxTail[2] = {};

    void pump() {
        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input_pending(seq, 1) > 0 && snd_seq_event_input(seq, &ev) >= 0 && ev) {
            const int p = ev->dest.port == port[1] ? 1 : 0;
            uint8_t buf[256];
            snd_midi_event_reset_decode(dec);
            const long n = snd_midi_event_decode(dec, buf, sizeof(buf), ev);
            for (long i = 0; i < n; ++i) {
                if (rxHead[p] - rxTail[p] >= kRx) break;
                rx[p][rxHead[p]++ % kRx] = buf[i];
            }
            ev = nullptr;
        }
    }
};
#else
struct HostMidi::Impl {};
#endif

HostMidi::HostMidi() = default;

HostMidi::~HostMidi() {
#ifdef OT_SIM_ALSA
    if (impl_) {
        for (auto* e : impl_->enc)
            if (e) snd_midi_event_free(e);
        if (impl_->dec) snd_midi_event_free(impl_->dec);
        snd_seq_close(impl_->seq);
        delete impl_;
    }
#endif
}

bool HostMidi::open() {
#ifdef OT_SIM_ALSA
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        std::fprintf(stderr, "sim midi: ALSA sequencer unavailable, MIDI disabled\n");
        return false;
    }
    snd_seq_set_client_name(seq, "OpenTracker");
    auto* im = new Impl;
    im->seq = seq;
    const char* names[2] = {"OpenTracker DIN", "OpenTracker USB"};
    const unsigned caps = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE |
                          SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_DUPLEX;
    for (int i = 0; i < 2; ++i) {
        im->port[i] = snd_seq_create_simple_port(seq, names[i], caps,
                                                 SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        snd_midi_event_new(256, &im->enc[i]);
    }
    snd_midi_event_new(256, &im->dec);
    snd_midi_event_no_status(im->dec, 1);   // always emit full status bytes
    impl_ = im;
    return true;
#else
    return false;
#endif
}

bool HostMidi::send(ot::MidiPortId port, const uint8_t* data, uint32_t len) {
#ifdef OT_SIM_ALSA
    if (!impl_) return true;   // swallowed
    const int p = port == ot::MidiPortId::kUsb ? 1 : 0;
    for (uint32_t i = 0; i < len; ++i) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        if (snd_midi_event_encode_byte(impl_->enc[p], data[i], &ev) == 1) {
            snd_seq_ev_set_source(&ev, impl_->port[p]);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output(impl_->seq, &ev);
        }
    }
    snd_seq_drain_output(impl_->seq);
    return true;
#else
    (void)port; (void)data; (void)len;
    return true;
#endif
}

uint32_t HostMidi::receive(ot::MidiPortId port, uint8_t* dst, uint32_t cap) {
#ifdef OT_SIM_ALSA
    if (!impl_) return 0;
    impl_->pump();
    const int p = port == ot::MidiPortId::kUsb ? 1 : 0;
    uint32_t n = 0;
    while (n < cap && impl_->rxTail[p] != impl_->rxHead[p]) dst[n++] = impl_->rx[p][impl_->rxTail[p]++ % Impl::kRx];
    return n;
#else
    (void)port; (void)dst; (void)cap;
    return 0;
#endif
}

}  // namespace sim
