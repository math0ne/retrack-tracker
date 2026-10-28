# Retrack desktop simulator: src/looper + src/platform + the OpenTracker host HAL in sim/src.
#   make -C sim   -> build/sim/looper-sim
#   make -C sim check  -> run the headless scripts in sim/scripts, each on a fresh SD folder
#                         (build/sim/check/sd_<script>)
#   make -C sim asan   -> build/sim-asan/looper-sim: the same with AddressSanitizer and
#                         UBSan (a bad memory access or undefined behaviour stops it there)
#   make -C sim soak   -> long random sessions (SEEDS="1 2 3", MINUTES=60) on the asan build
CXX      ?= g++
ROOT     := ..
OUT      ?= $(ROOT)/build/sim
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
OPT      ?= -O2
SAN      ?=
CXXFLAGS := -std=c++17 $(OPT) -g -Wall -Wextra -Wno-format-truncation -MMD $(SAN) \
            -I$(ROOT)/src -I$(ROOT)/src/platform -Isrc $(SDL_CFLAGS)
LDLIBS   := $(SDL_LIBS) -lpthread $(SAN)

APP_SRCS := $(shell find $(ROOT)/src/looper $(ROOT)/src/platform -name '*.cpp')
APP_OBJS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/obj/%.o,$(APP_SRCS))
# host_midi / sim_nvstore are kept for later use but not linked yet.
SIM_SRCS := $(addprefix src/,main.cpp canvas.cpp host_hal.cpp host_storage.cpp jpeg_decode.cpp panel.cpp png.cpp sdl_audio.cpp sw_display.cpp)
SIM_OBJS := $(patsubst src/%.cpp,$(OUT)/obj/sim/%.o,$(SIM_SRCS))
BIN      := $(OUT)/looper-sim

all: $(BIN)

$(OUT)/obj/%.o: $(ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/sim/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(APP_OBJS) $(SIM_OBJS)
	$(CXX) $^ $(LDLIBS) -o $@

check: $(BIN)
	@mkdir -p $(OUT)/check
	@set -e; for s in scripts/*.txt; do \
	    n=$$(basename $$s .txt); \
	    echo "== $$s"; \
	    rm -rf $(OUT)/check/sd_$$n; \
	    $(BIN) --script $$s --tone 330 --sd $(OUT)/check/sd_$$n --shot $(OUT)/check/$$n.png; \
	done

ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
asan:
	$(MAKE) OUT=$(ROOT)/build/sim-asan OPT=-O1 SAN="$(ASAN_FLAGS)"

SEEDS   ?= 1 2 3
MINUTES ?= 60
soak: asan
	@mkdir -p $(ROOT)/build/soak
	@set -e; for s in $(SEEDS); do \
	    rm -rf $(ROOT)/build/soak/sd_$$s; \
	    $(ROOT)/build/sim-asan/looper-sim --soak $$s --minutes $(MINUTES) --sd $(ROOT)/build/soak/sd_$$s \
	        --soak-log $(ROOT)/build/soak/seed_$$s.log; \
	done

-include $(APP_OBJS:.o=.d) $(SIM_OBJS:.o=.d)
.PHONY: all check asan soak
