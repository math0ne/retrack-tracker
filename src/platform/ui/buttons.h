// Front-panel button ids and UI event states (spec 05 §2.1.1, spec 06 §0.2).
#pragma once

#include <cstdint>

namespace ot {
namespace btn {
// F1..F8 are ids 0..7 (screen buttons under the 8 bottom labels).
constexpr uint8_t F1 = 0, F2 = 1, F3 = 2, F4 = 3, F5 = 4, F6 = 5, F7 = 6, F8 = 7;
// Module keys (spec 05 §1.5 button -> module map).
constexpr uint8_t InstrParams = 0x08;
constexpr uint8_t Performance = 0x09;
constexpr uint8_t File = 0x0a;
constexpr uint8_t Config = 0x0b;
constexpr uint8_t Master = 0x0c;
constexpr uint8_t SampleLoader = 0x0d;
constexpr uint8_t SamplePlayback = 0x0e;
constexpr uint8_t SampleEditor = 0x0f;
constexpr uint8_t SampleRecorder = 0x10;
constexpr uint8_t Song = 0x11;
// Editing keys (spec 06 §0.2).
constexpr uint8_t Note = 0x12;
constexpr uint8_t Instr = 0x13;
constexpr uint8_t Fx1 = 0x14;
constexpr uint8_t Fx2 = 0x15;
constexpr uint8_t Pattern = 0x16;
constexpr uint8_t Play = 0x17;
constexpr uint8_t Copy = 0x18;
constexpr uint8_t Insert = 0x19;
constexpr uint8_t Up = 0x1a;
constexpr uint8_t Delete = 0x1b;
constexpr uint8_t Rec = 0x1c;
constexpr uint8_t Shift = 0x1d;
constexpr uint8_t Left = 0x1e;
constexpr uint8_t Down = 0x1f;
constexpr uint8_t Right = 0x20;
constexpr uint8_t EncPush = 0x21;
constexpr uint8_t Aux = 0x22;
constexpr uint8_t Help = 0x2b;
// Power switch as a virtual button (slot 0x2a is unused in the key table,
// spec 05 §2.1.1). Shift + Power = screenshot (spec 05 §2.1.1, §3.4).
constexpr uint8_t Power = 0x2a;
constexpr uint8_t kCount = 44;   // spec 05 §2.1.1: 44 button slots

inline bool isFKey(uint8_t id) { return id <= F8; }
inline bool isModuleKey(uint8_t id) { return (id >= 0x08 && id <= 0x11) || id == Pattern || id == Help; }
// spec 05 §2.1.1: hold ticks of these ids arrive as presses (auto-repeat).
inline bool autoRepeats(uint8_t id) { return (id >= 0x19 && id <= 0x1b) || (id >= 0x1e && id <= 0x21); }
}  // namespace btn

// Event states delivered to screens (spec 06 §0.2).
enum : uint8_t { kRelease = 0, kPress = 1, kHold = 2 };

constexpr int kPadCount = 48;
constexpr int kPadCols = 12;
constexpr int kPadRows = 4;

}  // namespace ot
