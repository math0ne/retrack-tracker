// MIDI in the main loop: clock out (bytes the engine made, to USB; DIN goes
// out from the audio thread through Platform::midiDinRealtime when there is
// one), clock in (tempo, start / stop, phase), and learned controls (a note
// or a controller per action, from a footswitch or a controller).
#pragma once

#include <cstdint>

#include "looper/engine.h"
#include "looper/platform.h"

namespace looper {

enum class MidiAction : uint8_t {
    kRec,        // record / overdub the selected track (REC)
    kPlayStop,   // play / stop the selected track (F2)
    kUndo,
    kRedo,
    kReplace,    // punch-in the selected track
    kNextTrack,
    kPrevTrack,
    kPlayAll,    // the transport (Play)
    kCount
};
constexpr int kMidiActions = int(MidiAction::kCount);
extern const char* const kMidiActionName[kMidiActions];

// A learned message: a note (on, velocity > 0) or a controller (value >= 64,
// as footswitches send), on any channel.
struct MidiBinding {
    uint8_t type = 0;   // 0 none, 1 note, 2 controller
    uint8_t num = 0;
};
enum class MidiClock : uint8_t { kOff, kSend, kFollow };

class Midi {
public:
    void init(const Platform* p, Engine* e) {
        p_ = p;
        e_ = e;
    }
    // Once per main-loop pass: output, input, clock following. Learned actions
    // come out of nextAction().
    void service(MidiClock clock, const MidiBinding map[kMidiActions], uint32_t nowUs);
    bool nextAction(MidiAction& a);

    void learn(int action) { learn_ = action; }   // the next note / controller binds to it (-1: stop)
    int learning() const { return learn_; }
    bool learned(MidiBinding& b, int& action);     // a binding the last learn made (once)
    const char* lastMessage() const { return last_; }   // for the screen
    float followBpm() const { return estBpm_; }         // 0 until a clock is coming in
    bool clockIn() const { return clockSeen_; }

private:
    void byte(uint8_t b, MidiClock clock, const MidiBinding map[kMidiActions], uint32_t nowUs);
    void message(uint8_t status, uint8_t d1, uint8_t d2, const MidiBinding map[kMidiActions]);
    void tick(uint32_t nowUs);

    const Platform* p_ = nullptr;
    Engine* e_ = nullptr;
    // Channel-message parser (one per port, running status).
    struct Parser {
        uint8_t status = 0, d[2] = {}, n = 0;
    } in_[2];
    int curPort_ = 0;
    // Learned actions waiting for the app.
    MidiAction q_[8] = {};
    uint8_t qHead_ = 0, qTail_ = 0;
    int learn_ = -1, learnedAction_ = -1;
    MidiBinding learnedBinding_;
    char last_[24] = "-";
    // Clock in: the last ticks' times, the tick count since Start.
    static constexpr int kFit = 96;   // four beats
    uint32_t tickUs_[kFit] = {};
    int ticks_ = 0;                   // in tickUs_ (up to kFit)
    uint32_t sinceStart_ = 0;         // ticks since Start / Continue
    uint32_t lastTickUs_ = 0;
    float estBpm_ = 0.0f;
    bool clockSeen_ = false;
    int sentClock_ = -1;              // the clock-out setting the engine has
};

}  // namespace looper
