######################################################################
# MSX PICOVERSE PROJECT
# (c) 2026 Cristiano Goncalves
# The Retro Hacker
#
# Makefile - build script for the MSX PICOVERSE 2350 Carnivore2 firmware
#
# Builds the RP2350 firmware and the PC tool, and runs the host tests of the
# Carnivore2 core (needs a host C compiler).
######################################################################

# Shared version (can be overridden on the command line)
VERSION ?= v1.01

# Paths
PICO_DIR := pico/carnivore2
PICO_BUILD := $(PICO_DIR)/build
PICO_CONFIG := $(PICO_BUILD)/CMakeCache.txt
TOOL_DIR := tool
CORE_DIR := $(PICO_DIR)/core

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
CC ?= gcc

.PHONY: all firmware tool test clean

all: firmware tool

# Build options, e.g. 'make C2_COPY_TO_RAM=ON' or 'make C2_CLOCK_KHZ=210000'.
# Do not pass them as -D options: make treats -D as its own debug switch.
C2_COPY_TO_RAM ?= OFF
C2_CLOCK_KHZ ?= 300000
C2_CMAKE_OPTIONS := -DC2_COPY_TO_RAM=$(C2_COPY_TO_RAM) -DC2_SYS_CLOCK_KHZ=$(C2_CLOCK_KHZ)

# The CMake options are applied on every build (reconfiguring is quick).
firmware:
	@echo "Configuring Pico firmware (C2_COPY_TO_RAM=$(C2_COPY_TO_RAM) C2_CLOCK_KHZ=$(C2_CLOCK_KHZ))"
	@$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) -S "$(PICO_DIR)" -B "$(PICO_BUILD)" -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) $(C2_CMAKE_OPTIONS)
	@echo "Building Pico firmware"
	@$(CMAKE) --build "$(PICO_BUILD)"

tool: firmware
	@echo "Building PC tool"
	@"$(MAKE)" -C "$(TOOL_DIR)" VERSION=$(VERSION)

test:
	@echo "Running Carnivore2 core tests"
	$(CC) -std=c11 -Wall -Wextra -O1 -o tests/c2_core_test tests/c2_core_test.c $(CORE_DIR)/c2_core.c $(CORE_DIR)/c2_flash.c $(CORE_DIR)/c2_eeprom.c
	./tests/c2_core_test

clean:
	@echo "Cleaning firmware and tool"
	@"$(MAKE)" -C "$(TOOL_DIR)" clean VERSION=$(VERSION)
	@$(CMAKE) --build "$(PICO_BUILD)" --target clean || true
	@$(CMAKE) -E rm -rf "$(PICO_BUILD)"
	@$(CMAKE) -E rm -f tests/c2_core_test tests/c2_core_test.exe
