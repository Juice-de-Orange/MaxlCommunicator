# Host build of the firmware's portable modules.
#
# This compiles THE SAME sources as the ARM build -- not a copy, not a stub.
# That is the point: docs/decisions/0001-open-decisions.md D3 puts IRadioLink and
# IBlockStore in hal/ so that link/ depends on interfaces rather than on RadioLib.
# If link/ compiles on a plain host toolchain with no Arduino core in sight, the
# layering claim is demonstrated rather than asserted.
#
# make, g++ and python3 are all this needs -- no CMake, so it runs unchanged in
# the stock gcc image (see ../tools/hosttest.sh) and on a bare CI runner.
#
# What it cannot do: prove anything about the hardware. docs/test-plan.md -- every
# gate except 2.15 needs two devices on a bench.

BUILD    ?= build
FW_DIR   := ..
SRC_DIR  := $(FW_DIR)/src
REPO_DIR := $(FW_DIR)/..
VENDOR   := $(FW_DIR)/lib/tinycrypt

# The same discipline as platformio.ini's build_src_flags. Our code only.
WARNINGS  := -Wall -Wextra -Werror -Wshadow -Wdouble-promotion
# -DMAXL_SELF_TEST so link/self_test.cpp is compiled here too, not only in the
# debug firmware image. It is the code that runs at boot on the device (gate
# 2.15); exercising it only there would mean a bug in the self test itself shows
# up in the one place it is hardest to look at.
CXXFLAGS  := -std=c++17 -g -O1 $(WARNINGS) -I$(SRC_DIR) -I. -Ivendor -I$(BUILD)/generated \
             -I$(VENDOR)/include -DMAXL_SELF_TEST=1
CFLAGS    := -std=c11 -g -O1 -I$(VENDOR)/include

# tinycrypt is vendored third-party C (decision D4) and is built without -Werror:
# it is not our code and we do not patch it.
TC_SRC    := $(VENDOR)/src/aes_encrypt.c $(VENDOR)/src/ccm_mode.c $(VENDOR)/src/utils.c

# Globbed rather than listed: every .cpp in link/, ble/ and app/ is portable by
# construction (D3), so a new module that does NOT build on the host is exactly
# what this build exists to catch. A hand-written list would let one be left out.
#
# app/ is here for the same reason link/ is. CLAUDE.md 3 forbids dynamic
# allocation in both, and both talk to hardware only through hal/ interfaces --
# so both have to compile on a bare host toolchain, and if one day one of them
# does not, that is the layering breaking rather than a build quirk.
PORTABLE_SRC := $(wildcard $(SRC_DIR)/link/*.cpp) $(wildcard $(SRC_DIR)/ble/*.cpp) \
                $(wildcard $(SRC_DIR)/app/*.cpp) $(wildcard $(SRC_DIR)/ui/*.cpp)

# hal/ is NOT globbed, and that asymmetry is the point. Most of it talks to the
# Arduino core -- Wire, SPI, GxEPD2, RadioLib -- and could never build here. The
# files below are the deliberate exceptions: pure arithmetic and state machines,
# split out of their drivers precisely so the parts that can be wrong quietly
# are testable without a device on the bench.
#
# Each one is named, not matched, so adding a driver cannot drag the Arduino core
# into this build by accident. If a new file belongs here, it goes here by hand
# and the person adding it has to think about whether it really is portable.
PORTABLE_SRC += $(SRC_DIR)/hal/canvas.cpp \
                $(SRC_DIR)/hal/refresh_policy.cpp \
                $(SRC_DIR)/hal/press_detector.cpp \
                $(SRC_DIR)/hal/bme280_compensation.cpp \
                $(SRC_DIR)/hal/nmea.cpp
UNIT_SRC     := main.cpp $(wildcard unit/*.cpp)

# One binary per simulation, not one binary from all of them. Each carries its
# own main() and its own scenarios, and a scenario that hangs should not take the
# others down with it.
SIM_SRC      := $(wildcard sim/*.cpp)

# Sources live above this directory, so a naive $(BUILD)/%.o would resolve to
# build/../src/... and drop object files straight into the source tree. Rewrite
# the leading ../ into up/ so everything stays inside $(BUILD).
objpath = $(BUILD)/$(subst ../,up/,$1).o

PORTABLE_OBJ := $(foreach s,$(PORTABLE_SRC),$(call objpath,$s))
UNIT_OBJ     := $(foreach s,$(UNIT_SRC),$(call objpath,$s))
SIM_OBJ      := $(foreach s,$(SIM_SRC),$(call objpath,$s))
SIM_BINS     := $(foreach s,$(SIM_SRC),$(BUILD)/$(notdir $(basename $s)))
TC_OBJ       := $(foreach s,$(TC_SRC),$(call objpath,$s))

GENERATED := $(BUILD)/generated/test_vectors.h

BINARIES := $(BUILD)/unit_tests $(SIM_BINS)

.PHONY: all test clean
all: $(BINARIES)

test: all
	@echo "=== unit tests ==="
	@$(BUILD)/unit_tests
	@for sim in $(SIM_BINS); do \
		echo; \
		echo "=== $$(basename $$sim) ==="; \
		$$sim || exit 1; \
	done

# Test vectors are shared with the TypeScript bridge. The C++ side reads them as
# a generated header of byte arrays rather than parsing JSON -- a JSON parser has
# no business in a firmware test build, and the vectors are the same file either
# way.
$(GENERATED): $(wildcard $(REPO_DIR)/test-vectors/*.json) $(REPO_DIR)/tools/gen_vectors.py
	@mkdir -p $(dir $@)
	python3 $(REPO_DIR)/tools/gen_vectors.py --out $@

# Two rule pairs: one for sources beside this Makefile, one for sources above it
# (whose ../ has been rewritten to up/ in the object path).
$(BUILD)/%.cpp.o: %.cpp $(GENERATED)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/up/%.cpp.o: ../%.cpp $(GENERATED)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/up/%.c.o: ../%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/unit_tests: $(UNIT_OBJ) $(PORTABLE_OBJ) $(TC_OBJ)
	$(CXX) $^ -o $@

# Each simulation links only its own object, plus the firmware sources.
$(BUILD)/%: $(BUILD)/sim/%.cpp.o $(PORTABLE_OBJ) $(TC_OBJ)
	$(CXX) $^ -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
