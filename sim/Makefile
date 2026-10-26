# Tracker Looper desktop simulator: src/looper + src/platform + the OpenTracker host HAL in sim/src.
#   make -C sim   -> build/sim/looper-sim
#   make -C sim check  -> run the headless scripts in sim/scripts
CXX      ?= g++
ROOT     := ..
OUT      := $(ROOT)/build/sim
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-format-truncation -MMD \
            -I$(ROOT)/src -I$(ROOT)/src/platform -Isrc $(SDL_CFLAGS)
LDLIBS   := $(SDL_LIBS) -lpthread

APP_SRCS := $(shell find $(ROOT)/src/looper $(ROOT)/src/platform -name '*.cpp')
APP_OBJS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/obj/%.o,$(APP_SRCS))
# host_midi / host_storage / sim_nvstore are kept for later use but not linked yet.
SIM_SRCS := $(addprefix src/,main.cpp canvas.cpp host_hal.cpp jpeg_decode.cpp panel.cpp png.cpp sdl_audio.cpp sw_display.cpp)
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
	    echo "== $$s"; \
	    $(BIN) --script $$s --tone 330 --shot $(OUT)/check/$$(basename $$s .txt).png; \
	done

-include $(APP_OBJS:.o=.d) $(SIM_OBJS:.o=.d)
.PHONY: all check
