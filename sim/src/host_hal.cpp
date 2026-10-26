#include "host_hal.h"

#include "ui/buttons.h"

namespace sim {

namespace {
// spec 05 §2.1.1 hold timing: T = 333 ms, accel table.
constexpr uint32_t kHoldMs = 333;
constexpr uint16_t kHoldAccel[6] = {0, 100, 190, 235, 255, 270};
}  // namespace

void SimInput::push(ot::InputEvent::Type t, uint8_t id, uint8_t vel, int8_t d) {
    ot::InputEvent ev;
    ev.type = t;
    ev.id = id;
    ev.velocity = vel;
    ev.delta = d;
    ev.timeMs = clk_.millis();
    q_.push_back(ev);
}

void SimInput::button(uint8_t id, bool down) {
    std::lock_guard<std::mutex> lk(mu_);
    if (id >= 64 || down == buttonDown(id)) return;
    if (down) {
        buttons_ |= 1ull << id;
        holdNext_[id] = clk_.millis() + kHoldMs;
        holdLevel_[id] = 0;
    } else {
        buttons_ &= ~(1ull << id);
    }
    push(down ? ot::InputEvent::kButtonDown : ot::InputEvent::kButtonUp, id);
}

void SimInput::pad(uint8_t p, bool down, uint8_t velocity) {
    std::lock_guard<std::mutex> lk(mu_);
    if (p >= 48 || down == padDown(p)) return;
    if (down) pads_ |= 1ull << p;
    else pads_ &= ~(1ull << p);
    push(down ? ot::InputEvent::kPadDown : ot::InputEvent::kPadUp, p, down ? velocity : 0);
}

void SimInput::encoder(int delta) {
    std::lock_guard<std::mutex> lk(mu_);
    while (delta) {
        const int d = delta > 127 ? 127 : (delta < -127 ? -127 : delta);
        push(ot::InputEvent::kEncoder, 0, 0, int8_t(d));
        delta -= d;
    }
}

void SimInput::service() {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t now = clk_.millis();
    for (uint8_t id = 0; id < 64; ++id) {
        if (!buttonDown(id) || !ot::btn::autoRepeats(id)) continue;
        if (int32_t(now - holdNext_[id]) >= 0) {
            push(ot::InputEvent::kButtonDown, id);
            if (holdLevel_[id] < 5) ++holdLevel_[id];
            holdNext_[id] = now + (kHoldMs - kHoldAccel[holdLevel_[id]]);
        }
    }
}

bool SimInput::poll(ot::InputEvent& ev) {
    service();
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    ev = q_.front();
    q_.pop_front();
    return true;
}

}  // namespace sim
