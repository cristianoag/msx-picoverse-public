######################################################################
# MSX PICOVERSE PROJECT
# (c) 2026 Cristiano Goncalves
# The Retro Hacker
#
# Makefile - build script for loadrom.pio
#
# Builds both the RP2040 firmware and the PC tool.
######################################################################

# Shared version (can be overridden on the command line)
VERSION ?= v2.63

# Paths
PICO_DIR := pico/loadrom
PICO_BUILD := $(PICO_DIR)/build
PICO_CONFIG := $(PICO_BUILD)/build.ninja
KEYBOARD_DIR := pico/keyboard
KEYBOARD_BUILD := $(KEYBOARD_DIR)/build
KEYBOARD_CONFIG := $(KEYBOARD_BUILD)/build.ninja
MIDI_DIR := pico/midi
MIDI_BUILD := $(MIDI_DIR)/build
MIDI_CONFIG := $(MIDI_BUILD)/build.ninja
MIDIPAC_DIR := pico/midipac
MIDIPAC_BUILD := $(MIDIPAC_DIR)/build
MIDIPAC_CONFIG := $(MIDIPAC_BUILD)/build.ninja
JOYSTICK_DIR := pico/joystick
JOYSTICK_BUILD := $(JOYSTICK_DIR)/build
JOYSTICK_CONFIG := $(JOYSTICK_BUILD)/build.ninja
TOOL_DIR := tool

# CMake options
CMAKE ?= c:/Users/Cristiano/.pico-sdk/cmake/v3.29.9/bin/cmake
# Use Ninja on Windows to avoid NMake and missing MinGW make.
ifeq ($(OS),Windows_NT)
	CMAKE_GENERATOR ?= Ninja
	CMAKE_MAKE_PROGRAM ?= C:/Users/Cristiano/.pico-sdk/ninja/v1.12.1/ninja.exe
	ifneq ($(wildcard C:/msys64/ucrt64/bin/python.exe),)
		PYTHON3_EXECUTABLE ?= C:/msys64/ucrt64/bin/python.exe
	endif
else
	CMAKE_GENERATOR ?=
endif
CMAKE_GENERATOR_FLAG := $(if $(CMAKE_GENERATOR),-G "$(CMAKE_GENERATOR)",)
CMAKE_BUILD_TYPE ?= Release
CMAKE_MAKE_PROGRAM_FLAG := $(if $(CMAKE_MAKE_PROGRAM),-DCMAKE_MAKE_PROGRAM="$(CMAKE_MAKE_PROGRAM)",)
PYTHON3_EXECUTABLE_FLAG := $(if $(PYTHON3_EXECUTABLE),-DPython3_EXECUTABLE="$(PYTHON3_EXECUTABLE)",)

# Make options
MAKE ?= make



.PHONY: all firmware keyboard midi midipac joystick tool clean

all: firmware keyboard midi midipac joystick tool

firmware:
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) $(PYTHON3_EXECUTABLE_FLAG) -S "$(PICO_DIR)" -B "$(PICO_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
	@echo "Building Pico firmware"
	@$(CMAKE) --build "$(PICO_BUILD)"

keyboard:
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) $(PYTHON3_EXECUTABLE_FLAG) -S "$(KEYBOARD_DIR)" -B "$(KEYBOARD_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
	@echo "Building Keyboard firmware"
	@$(CMAKE) --build "$(KEYBOARD_BUILD)"

midi:
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) $(PYTHON3_EXECUTABLE_FLAG) -S "$(MIDI_DIR)" -B "$(MIDI_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
	@echo "Building MIDI firmware"
	@$(CMAKE) --build "$(MIDI_BUILD)"

midipac:
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) $(PYTHON3_EXECUTABLE_FLAG) -S "$(MIDIPAC_DIR)" -B "$(MIDIPAC_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
	@echo "Building MIDI-PAC firmware"
	@$(CMAKE) --build "$(MIDIPAC_BUILD)"

joystick:
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) $(PYTHON3_EXECUTABLE_FLAG) -S "$(JOYSTICK_DIR)" -B "$(JOYSTICK_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
	@echo "Building Joystick firmware"
	@$(CMAKE) --build "$(JOYSTICK_BUILD)"

tool:
	@echo "Building PC tool"
	@"$(MAKE)" -C "$(TOOL_DIR)" VERSION=$(VERSION)

clean:
	@echo "Cleaning firmware, keyboard, midi, midipac, joystick, and tool"
	@"$(MAKE)" -C "$(TOOL_DIR)" clean VERSION=$(VERSION)
	@$(CMAKE) --build "$(PICO_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(PICO_BUILD)"
	@$(CMAKE) --build "$(KEYBOARD_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(KEYBOARD_BUILD)"
	@$(CMAKE) --build "$(MIDI_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(MIDI_BUILD)"
	@$(CMAKE) --build "$(MIDIPAC_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(MIDIPAC_BUILD)"
	@$(CMAKE) --build "$(JOYSTICK_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(JOYSTICK_BUILD)"
