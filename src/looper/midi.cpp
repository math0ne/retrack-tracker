#include "looper/midi.h"

#include <cstdio>

namespace looper {

const char* const kMidiActionName[kMidiActions] = {"Rec / dub", "Play / stop", "Undo", "Redo",
                                                   "Replace",   "Next track",  "Prev track", "Play all"};

void Midi::service(MidiClock clock, const MidiBinding map[kMidiActions], uint32_t nowUs) {
    ot::IMidiPort* port = p_ ? p_->midi : nullptr;
    // Out: the engine's real-time bytes (clock) to USB, and to DIN too when the
    // audio thread does not send them there itself.
    uint8_t b;
    while (e_->popRealtime(b)) {
        if (!port) continue;
        port->send(ot::MidiPortId::kUsb, &b, 1);
        if (!p_->midiDinRealtime) port->send(ot::MidiPortId::kDin, &b, 1);
    }
    if (int(clock) != sentClock_) {
        sentClock_ = int(clock);
        e_->post(Engine::kClockOut, 0, clock == MidiClock::kSend ? 1 : 0);
    }
    if (!port) return;
    // In, from both ports.
    uint8_t buf[32];
    for (int pi = 0; pi < 2; ++pi) {
        curPort_ = pi;
        uint32_t n;
        while ((n = port->receive(pi ? ot::MidiPortId::kUsb : ot::MidiPortId::kDin, buf, sizeof buf)) > 0)
            for (uint32_t i = 0; i < n; ++i) byte(buf[i], clock, map, nowUs);
    }
    // No clock for half a second: not following any more.
    if (clockSeen_ && nowUs - lastTickUs_ > 500000u) {
        clockSeen_ = false;
        estBpm_ = 0.0f;
        ticks_ = 0;
    }
}

void Midi::byte(uint8_t b, MidiClock clock, const MidiBinding map[kMidiActions], uint32_t nowUs) {
    if (b >= 0xF8) {   // real-time, between any bytes
        if (clock != MidiClock::kFollow) return;
        switch (b) {
            case 0xF8: tick(nowUs); break;
            case 0xFA:   // Start: everything from bar 1, the tick count from 0
            case 0xFB:   // Continue (no song position here): the same
                sinceStart_ = 0;
                if (!e_->running()) e_->post(Engine::kTransport);
                break;
            case 0xFC:
                if (e_->running()) e_->post(Engine::kTransport);
                break;
            default: break;
        }
        return;
    }
    Parser& p = in_[curPort_];
    if (b & 0x80) {   // status
        if (b >= 0xF0) {   // system common / sysex: not used, and it ends running status
            p.status = 0;
            return;
        }
        p.status = b;
        p.n = 0;
        return;
    }
    if (!p.status) return;
    p.d[p.n++] = b;
    const uint8_t hi = p.status & 0xF0;
    const uint8_t need = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
    if (p.n < need) return;
    p.n = 0;   // running status: the next data byte starts a new message
    message(p.status, p.d[0], need > 1 ? p.d[1] : 0, map);
}

void Midi::message(uint8_t status, uint8_t d1, uint8_t d2, const MidiBinding map[kMidiActions]) {
    const uint8_t hi = status & 0xF0;
    uint8_t type = 0;
    if (hi == 0x90 && d2 > 0) type = 1;          // note on
    else if (hi == 0xB0 && d2 >= 64) type = 2;   // controller pressed (footswitch down)
    if (hi == 0x90 || hi == 0xB0)
        std::snprintf(last_, sizeof last_, "%s %u = %u", hi == 0x90 ? "Note" : "CC", unsigned(d1), unsigned(d2));
    if (!type) return;
    if (learn_ >= 0) {
        learnedBinding_.type = type;
        learnedBinding_.num = d1;
        learnedAction_ = learn_;
        learn_ = -1;
        return;
    }
    for (int a = 0; a < kMidiActions; ++a)
        if (map[a].type == type && map[a].num == d1) {
            const uint8_t nh = uint8_t((qHead_ + 1) % (sizeof q_ / sizeof q_[0]));
            if (nh != qTail_) {
                q_[qHead_] = MidiAction(a);
                qHead_ = nh;
            }
        }
}

bool Midi::nextAction(MidiAction& a) {
    if (qTail_ == qHead_) return false;
    a = q_[qTail_];
    qTail_ = uint8_t((qTail_ + 1) % (sizeof q_ / sizeof q_[0]));
    return true;
}

bool Midi::learned(MidiBinding& b, int& action) {
    if (learnedAction_ < 0) return false;
    b = learnedBinding_;
    action = learnedAction_;
    learnedAction_ = -1;
    return true;
}

void Midi::tick(uint32_t nowUs) {
    // The main loop reads the ports, so a tick's time is off by up to a pass
    // (~20 ms): the tempo is a least-squares line through the last four beats
    // of ticks, and a slow correction keeps the bar clock on the tick count.
    clockSeen_ = true;
    lastTickUs_ = nowUs;
    ++sinceStart_;
    if (ticks_ == kFit) {
        for (int i = 1; i < kFit; ++i) tickUs_[i - 1] = tickUs_[i];
        --ticks_;
    }
    tickUs_[ticks_++] = nowUs;
    // A first estimate a quarter beat after Start (so the loops are not long at
    // the old tempo), every quarter beat for the first beat, then once a beat.
    if (ticks_ < 6) return;
    if (sinceStart_ < 24 ? sinceStart_ % 6 != 0 : sinceStart_ % 24 != 0) return;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double t0 = tickUs_[0];
    for (int i = 0; i < ticks_; ++i) {
        const double y = double(tickUs_[i]) - t0;
        sx += i;
        sy += y;
        sxx += double(i) * i;
        sxy += double(i) * y;
    }
    const double nn = ticks_, den = nn * sxx - sx * sx;
    if (den <= 0) return;
    const double usPerTick = (nn * sxy - sx * sy) / den;
    if (usPerTick <= 0) return;
    const float bpm = float(60e6 / (usPerTick * 24.0));
    estBpm_ = bpm;
    // Phase: where the bar clock should be (ticks since Start) against where it is.
    float corr = 0.0f;
    if (e_->running() && e_->beatFrames()) {
        const float want = float(sinceStart_) / 24.0f;
        const float have = float(e_->transportFrame()) / float(e_->beatFrames());
        corr = (want - have) * 0.1f;   // a tenth of the error each beat
        corr = corr < -0.02f ? -0.02f : (corr > 0.02f ? 0.02f : corr);
    }
    e_->post(Engine::kExtTempo, 0, 0, uint32_t(double(bpm) * (1.0 + double(corr)) * 65536.0));
}

}  // namespace looper
