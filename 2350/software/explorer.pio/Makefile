######################################################################
# MSX PICOVERSE 2350 Explorer - Aggregate Makefile                   #
#                                                                    #
# Invokes the individual build systems for each Explorer component.  #
# Build order matters because later stages embed artifacts produced  #
# by earlier ones, so the tool target always runs last. The openMSX  #
# add-on does not depend on the other stages.                        #
######################################################################

# Shared version (can be overridden on the command line)
VERSION ?= v2.57

# Sub-project locations
MSX_DIR       := msx
PICO_DIR      := pico/explorer
PICO_BUILD    := $(PICO_DIR)/build
PICO_CONFIG   := $(PICO_BUILD)/CMakeCache.txt
TOOL_DIR      := tool
OPENMSX_DIR   := openmsx
WIFI_CONFIG_DIR := ../wifi/config

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

.PHONY: all msx pico wifi-config tool openmsx clean

all: tool openmsx

msx:
	"$(MAKE)" -C $(MSX_DIR) VERSION=$(VERSION)

pico: msx
	$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) -S $(PICO_DIR) -B $(PICO_BUILD) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DEXPLORER_VERSION:STRING="$(VERSION)"
	$(CMAKE) --build $(PICO_BUILD)

$(PICO_CONFIG):
	$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_MAKE_PROGRAM_FLAG) -S $(PICO_DIR) -B $(PICO_BUILD) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DEXPLORER_VERSION:STRING="$(VERSION)"

wifi-config:
	"$(MAKE)" -C $(WIFI_CONFIG_DIR)

tool: pico wifi-config
	"$(MAKE)" -C $(TOOL_DIR) VERSION=$(VERSION)

# openMSX add-on (Tcl script + extension); independent of the other stages.
openmsx:
	"$(MAKE)" -C $(OPENMSX_DIR) VERSION=$(VERSION)

clean:
	-"$(MAKE)" -C $(MSX_DIR) clean VERSION=$(VERSION)
	-@$(CMAKE) --build $(PICO_BUILD) --target clean
	-"$(MAKE)" -C $(WIFI_CONFIG_DIR) clean
	-"$(MAKE)" -C $(TOOL_DIR) clean VERSION=$(VERSION)
	-@$(CMAKE) -E rm -rf $(PICO_BUILD)
