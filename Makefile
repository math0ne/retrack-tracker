# Tracker Looper firmware for the original Polyend Tracker hardware (MK66FX1M0).
# make            -> build/looper.hex and build/PolyendTracLooperv<ver digits>.ptf (24 chars),
#                    also at build/sdcard/Firmware/ (copy that Firmware folder onto the SD card)
# make SELFTEST=1 -> build/selftest/PolyendTracLooperST1.ptf, the OpenTracker hardware selftest
# make sim        -> build/sim/looper-sim (desktop simulator, sim/README.md)
VERSION   ?= 0.1
BUILD     := build
VERNUM    := $(subst .,,$(VERSION))
ifeq ($(SELFTEST),1)
BUILD     := build/selftest
DEFS_EXTRA := -DOT_FORCE_SELFTEST=1
PTF_NAME  := PolyendTracLooperST1.ptf
else
PTF_NAME  := PolyendTracLooperv$(VERNUM).ptf
endif
PTF       := $(BUILD)/$(PTF_NAME)
# The update bootloader ignores image names that are not exactly 24 characters,
# and the stock updater lists only Firmware/PolyendTrac*.ptf.
ifneq ($(shell printf '%s' '$(PTF_NAME)' | wc -c | tr -d ' '),24)
$(error image name '$(PTF_NAME)' must be exactly 24 characters)
endif
# USB identity: the pid.codes test ID, as in OpenTracker (not PJRC's, not Polyend's).
USB_ID    := -DOT_USB_VID=0x1209 -DOT_USB_PID=0x0001
CORE      := third_party/teensy3
CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
CXX       := $(CROSS)g++
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size

CPUFLAGS  := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
DEFS      := -D__MK66FX1M0__ -DF_CPU=180000000 -DUSB_MIDI_SERIAL -DLAYOUT_US_ENGLISH \
             -DF_BUS=90000000 -DSERIAL5_TX_BUFFER_SIZE=512 -DSERIAL5_RX_BUFFER_SIZE=512 \
             -DTEENSYDUINO=159 -DARDUINO=10819 -DOT_VERSION=\"looper-$(VERSION)\" -DOT_TARGET_DEVICE=1 \
             $(USB_ID) $(DEFS_EXTRA)
INCS      := -I$(CORE) -Isrc -Isrc/platform -Isrc/hal -Ithird_party/fatfs
COMMON    := $(CPUFLAGS) $(DEFS) $(INCS) -O2 -g -ffunction-sections -fdata-sections -Wall -MMD
CFLAGS    := $(COMMON) -std=gnu11
CXXFLAGS  := $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti -felide-constructors -Wno-psabi
LDFLAGS   := $(CPUFLAGS) -O2 -Wl,--gc-sections,--defsym=__rtc_localtime=0 -T$(CORE)/mk66fx1m0.ld \
             --specs=nano.specs -u _printf_float
LIBS      := -lm -lstdc++

# Sorted sources, and __DATE__ / __TIME__ from the last commit, so any machine
# (or CI) builds a byte-identical image from the same commit.
CORE_C    := $(sort $(wildcard $(CORE)/*.c))
CORE_CXX  := $(sort $(filter-out $(CORE)/main.cpp,$(wildcard $(CORE)/*.cpp)))
APP_C     := $(sort $(shell find src -name '*.c')) third_party/fatfs/ff.c third_party/fatfs/ffunicode.c
APP_CXX   := $(sort $(shell find src -name '*.cpp'))
export SOURCE_DATE_EPOCH ?= $(shell git log -1 --format=%ct 2>/dev/null)
OBJS      := $(patsubst %,$(BUILD)/%.o,$(CORE_C) $(CORE_CXX) $(APP_C) $(APP_CXX))

SDCARD    := $(BUILD)/sdcard/Firmware/$(PTF_NAME)

all: $(BUILD)/looper.hex $(PTF) $(SDCARD)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/looper.elf: $(OBJS)
	$(CXX) $(LDFLAGS) $^ -L$(CORE) $(LIBS) -o $@ -Wl,-Map=$(BUILD)/looper.map
	$(SIZE) $@

$(BUILD)/looper.hex: $(BUILD)/looper.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

# tools/check_image.py refuses an image whose flash-security bytes differ from
# stock (a locked chip) or that leaves the stock flash range.
$(PTF): $(BUILD)/looper.hex
	python3 tools/check_image.py $<
	cp $< $@

# build/sdcard mirrors the SD card: copy its Firmware folder onto the card's root.
# ot_restore.txt names the image, so a Tracker running the looper (or OpenTracker)
# flashes it at the next power-on; it renames the file to ot_restore.done after.
$(SDCARD): $(PTF)
	@mkdir -p $(dir $@)
	cp $< $@
	printf '%s\r\n' '$(PTF_NAME)' > $(dir $@)ot_restore.txt

clean:
	rm -rf build

sim:
	$(MAKE) -C sim

-include $(OBJS:.o=.d)
.PHONY: all clean sim
