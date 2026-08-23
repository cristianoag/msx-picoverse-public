######################################################################
# MSX PICOVERSE PROJECT
# (c) 2026 Cristiano Goncalves
# The Retro Hacker
#
# Makefile - build script for loadrom
#
# Builds both the RP2350 firmware and the PC tool.
######################################################################

# Shared version (can be overridden on the command line)
VERSION ?= v2.74

# Paths
PICO_DIR := pico/loadrom
PICO_BUILD := $(PICO_DIR)/build
PICO_CONFIG := $(PICO_BUILD)/CMakeCache.txt
OPL4_DIR := pico/opl4
OPL4_BUILD := $(OPL4_DIR)/build
OPL4_CONFIG := $(OPL4_BUILD)/CMakeCache.txt
# Second OPL4 payload: same sources, generated at 22.05 kHz (one chip sample per
# two native sample periods) for high-polyphony songs that exceed the 44.1 kHz
# real-time budget. Selected with the tool's --22khz option.
OPL4_22K_BUILD := $(OPL4_DIR)/build22k
OPL4_22K_CONFIG := $(OPL4_22K_BUILD)/CMakeCache.txt
MSXAUDIO_DIR := pico/msxaudio
MSXAUDIO_BUILD := $(MSXAUDIO_DIR)/build
MSXAUDIO_CONFIG := $(MSXAUDIO_BUILD)/CMakeCache.txt
TOOL_DIR := tool
WIFI_DIR := ../wifi
WIFI_BIOS_DIR := $(WIFI_DIR)/bios

# CMake options
CMAKE ?= c:/Users/Cristiano/.pico-sdk/cmake/v3.29.9/bin/cmake
# Use Ninja on Windows to avoid NMake and missing MinGW make.
ifeq ($(OS),Windows_NT)
	CMAKE_GENERATOR ?= Ninja
	CMAKE_MAKE_PROGRAM ?= C:/Users/Cristiano/.pico-sdk/ninja/v1.12.1/ninja.exe
else
	CMAKE_GENERATOR ?=
endif
CMAKE_GENERATOR_FLAG := $(if $(CMAKE_GENERATOR),-G "$(CMAKE_GENERATOR)",)
CMAKE_BUILD_TYPE ?= Release
CMAKE_MAKE_PROGRAM_FLAG := $(if $(CMAKE_MAKE_PROGRAM),-DCMAKE_MAKE_PROGRAM="$(CMAKE_MAKE_PROGRAM)",)

# Make options
MAKE ?= make

.PHONY: all bios updater firmware opl4 opl4_22k msxaudio tool clean

all: bios updater firmware opl4 opl4_22k msxaudio tool

bios:
	@echo "Building WiFi BIOS ROM"
	@"$(MAKE)" -C "$(WIFI_BIOS_DIR)"

firmware: $(PICO_CONFIG)
	@echo "Building Pico firmware"
	@$(CMAKE) --build "$(PICO_BUILD)"

$(PICO_CONFIG):
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) -S "$(PICO_DIR)" -B "$(PICO_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)

opl4: $(OPL4_CONFIG)
	@echo "Building standalone OPL4 firmware"
	@$(CMAKE) --build "$(OPL4_BUILD)"

$(OPL4_CONFIG):
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) -S "$(OPL4_DIR)" -B "$(OPL4_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)

opl4_22k: $(OPL4_22K_CONFIG)
	@echo "Building standalone OPL4 firmware (22.05 kHz)"
	@$(CMAKE) --build "$(OPL4_22K_BUILD)"

$(OPL4_22K_CONFIG):
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) -S "$(OPL4_DIR)" -B "$(OPL4_22K_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DOPL4_TARGET_NAME=opl4_22k -DOPL4_SAMPLE_RATE=22050 -DOPL4_OUTPUT_STEP=2

msxaudio: $(MSXAUDIO_CONFIG)
	@echo "Building standalone MSX-AUDIO firmware"
	@$(CMAKE) --build "$(MSXAUDIO_BUILD)"

$(MSXAUDIO_CONFIG):
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) -S "$(MSXAUDIO_DIR)" -B "$(MSXAUDIO_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)

tool: bios opl4 opl4_22k msxaudio
	@echo "Building PC tool"
	@"$(MAKE)" -C "$(TOOL_DIR)" VERSION=$(VERSION)

clean:
	@echo "Cleaning firmware and tool"
	@"$(MAKE)" -C "$(WIFI_BIOS_DIR)" clean || true
	@"$(MAKE)" -C "$(TOOL_DIR)" clean VERSION=$(VERSION)
	@$(CMAKE) --build "$(PICO_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(PICO_BUILD)"
	@$(CMAKE) --build "$(OPL4_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(OPL4_BUILD)"
	@$(CMAKE) --build "$(MSXAUDIO_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(MSXAUDIO_BUILD)"
