// OpenTracker -- device HAL entry points: board bring-up (spec 01 §0) and the
// driver singletons that implement src/core/hal_api.h.
#pragma once

#include <stdint.h>
#include "hal_api.h"

namespace ot {
namespace hal {

class Ft81xDisplay;
class Sgtl5000Audio;
class PanelInput;
class PadLeds;
class SdStorage;
class MidiPorts;
class Si4703Radio;

class SystemClock final : public IClock {
public:
    uint32_t micros() const override;
    uint32_t millis() const override;
};

struct BootReport {
    bool clocksOk = false;      // SIM_CLKDIV1 == spec value (core 180 / bus 90)
    bool sdramOk = false;       // quick self-test after init
    bool codecOk = false;
    bool displayOk = false;
    bool buttonsOk = false;
    bool padsOk = false;
    bool ledsOk = false;
    bool padBusAck = false;     // TCA8418 0x34 ACKed on the pad bus (board::kPadBusIndex)
    bool sdMounted = false;
    bool sdPresent = false;     // card-detect PTD0 LOW at boot
    uint32_t sdramErrors = 0;
    uint8_t stage = 0;          // last boardInit() step started (spec 01 §0 numbering)
    uint8_t displayId = 0;      // last FT81x REG_ID read (0x7C expected)
    uint8_t displayAttempt = 0; // 0 = internal osc (stock), 1 = CLKEXT fallback, 2 = failed
    uint16_t codecId = 0;       // SGTL5000 CHIP_ID (0xA0xx expected, 0 = no answer)
};

// Status LED (spec 01 §10: PTC19, HIGH = off). ON solid from reset until
// boardInit() returns, so a hang during bring-up shows as a lit LED.
void statusLed(bool on);
// Blink-code failure mask for hw::statusLedOn() (bit n = code n+1, hw_math.h).
uint32_t bootFailMask(const BootReport& r);

// Everything the application needs (ARCHITECTURE.md: core sees only interfaces).
struct HalSet {
    IDisplay* display;
    IAudioOut* audio;
    IStorage* storage;
    IMidiPort* midi;
    IInput* input;
    ILeds* leds;
    IClock* clock;
    void (*service)();          // call from the main loop (card detect, LEDs, power switch)
};

// Rail enable + SD card only (run first so the SD restore trigger works even if a
// later driver fails). Idempotent; boardInit() calls it if needed.
const BootReport& boardEarlyInit();
// spec 01 §0 bring-up order (rest). Never drives PTA1 (spec 01 §10).
const BootReport& boardInit();
const BootReport& bootReport();
HalSet& halSet();
void service();

// spec 01 §2: SDRAM test over [base, base+bytes). Destroys contents.
bool sdramTest(uint32_t bytes, uint32_t* errors);

// spec 01 §10 power switch (PTC3, LOW = pressed). Treated as a momentary
// power button (spec 05 power-button handler: press/release callbacks, 1500 ms
// hold countdown). service() delivers it as button 0x2a (core btn::Power) and
// powers off after a 1500 ms hold that did not start with Shift held.
bool powerSwitchOn();
// Called once by powerOff() before the peripherals stop (e.g. the app's
// workspace autosave). Main-loop context; audio is already stopped.
void setPowerOffHook(void (*fn)());
[[noreturn]] void powerOff();   // hook, then softOff()
// Peripherals off, then a low-power wait (VLPS, wake on the power switch) and a
// system reset once the switch is pressed again (spec 01 §10).
[[noreturn]] void softOff();
// spec 01 §10 boot gate: EEPROM power flag 0 (switched off with the power
// button) -> stay off until the switch is pressed. Call after the ot_restore check.
void bootPowerGate();

// Recorder input routing (HalRefs::selectInput, spec 05 appendix C §2.4):
// 0 line in, 1 FM radio, 2 mic low gain, 3 mic high gain. `gain` is applied
// digitally by the app (core app/capture.h), so it is not used here.
bool selectInput(void* ctx, uint8_t source, uint8_t gain);
// Line input block for HalRefs::lineIn (I2S RX, spec 01 §5.3).
const int16_t* lineIn(void* ctx);
// Recorder input diagnostics (app_main.cpp, 1 Hz while the recorder is open):
// on = start/stop the raw RX peak tracking; inputDiagLog() logs one "input:"
// line (raw RX peak, codec ADC routing readback, radio switch and Si4703
// status). Main-loop context (codec reads over I2C0).
void inputDiagEnable(bool on);
void inputDiagLog();

Ft81xDisplay& display();
Sgtl5000Audio& audio();
PanelInput& input();
PadLeds& leds();
SdStorage& storage();
MidiPorts& midi();
Si4703Radio& radio();

}  // namespace hal
}  // namespace ot
