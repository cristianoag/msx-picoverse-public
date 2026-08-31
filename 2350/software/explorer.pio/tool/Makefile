######################################################################
# MSX PICOVERSE PROJECT                                               
# (c) 2025 Cristiano Goncalves                                        
# The Retro Hacker                                                    
#                                                                      
# Makefile - build script for the MSX PICOVERSE 2350 Explorer tool    
#                                                                      
# This Makefile compiles the Explorer utility, regenerates the embedded 
# Raspberry Pi Pico firmware header, and packages the executable.      
######################################################################

# Toolchain configuration
CC      := gcc
XXD     := xxd

# Directory layout
SRCDIR  := src
BINDIR  := build
DISDIR  := dist
UTLDIR  := utils

# Build flags
VERBOSE ?= --verbose

# Debug control: set DEBUG=1 on the make command line to enable debug build
# Example: `make DEBUG=1`
DEBUG ?= 0

ifeq ($(DEBUG),1)
CCFLAGS := -g -DDEBUG
else
CCFLAGS := -g
endif

# Application metadata
VERSION ?= v2.48

# Project files
SOURCES := explorer.c
OUTFILE := explorer.exe

# Firmware assets (generated header embeds the Pico firmware image)
PICOBIN   := ../pico/explorer/build/explorer.bin
PICOBIN_H := $(SRCDIR)/explorer.h
MSXMENU := ../msx/dist/menu.rom
MSXMENU_H := $(SRCDIR)/menu.h
NEXTOR := ../resources/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM
NEXTOR_H := $(SRCDIR)/nextor.h
WIFICONFIG := ../../wifi/config/ESP8266_config.rom
WIFICONFIG_H := $(SRCDIR)/wifibios.h
WIFIBIOS := ../../wifi/bios/ESP8266P.rom
WIFIBIOS_H := $(SRCDIR)/esp8266p_rom.h
FMPAC_BIOS := ../../loadrom.pio/fmpac/FMPCCMFC.BIN
FMPAC_BIOS_H := $(SRCDIR)/fmpac_bios.h
SFG_BIOS := ../resources/SFG_64K.ROM
SFG_BIOS_H := $(SRCDIR)/sfg_bios.h

# Helpers
RM := rm -f

.PHONY: all compile package clean

all: clean compile package

compile: $(BINDIR)/$(OUTFILE)

package: $(DISDIR)/explorer.exe

$(BINDIR)/$(OUTFILE): $(BINDIR) $(SRCDIR)/$(SOURCES) $(PICOBIN_H) $(PICOBIN) $(MSXMENU_H) $(MSXMENU) $(NEXTOR_H) $(NEXTOR) $(WIFICONFIG_H) $(WIFICONFIG) $(WIFIBIOS_H) $(WIFIBIOS) $(FMPAC_BIOS_H) $(FMPAC_BIOS) $(SFG_BIOS_H) $(SFG_BIOS)
	@echo "Compiling $@"
	$(CC) $(CCFLAGS) -DAPP_VERSION=\"$(VERSION)\" $(SRCDIR)/$(SOURCES) -o $@

$(PICOBIN_H): $(PICOBIN)
	@echo "Embedding Pico firmware into header"
	$(UTLDIR)/$(XXD) -i "$<" $@

$(MSXMENU_H): $(MSXMENU)
	@echo "Embedding MSX menu ROM into header"
	$(UTLDIR)/$(XXD) -i "$<" $@

$(NEXTOR_H): $(NEXTOR)
	@echo "Embedding Nextor ROM into header"
	$(UTLDIR)/$(XXD) -i "$<" $@

$(WIFICONFIG_H): $(WIFICONFIG)
	@echo "Embedding WiFi configuration ROM into header"
	$(UTLDIR)/$(XXD) -i "$<" $@

$(WIFIBIOS_H): $(WIFIBIOS)
	@echo "Embedding ESP8266P WiFi BIOS into header"
	$(UTLDIR)/$(XXD) -i "$<" $@

$(FMPAC_BIOS_H): $(FMPAC_BIOS)
	@echo "Embedding FM-PAC BIOS into header"
	$(UTLDIR)/$(XXD) -i "$<" $@

$(SFG_BIOS_H): $(SFG_BIOS)
	@echo "Embedding SFG BIOS into header"
	$(UTLDIR)/$(XXD) -i "$<" $@

$(BINDIR):
	@mkdir $@

$(DISDIR):
	@mkdir $@

$(DISDIR)/explorer.exe: $(DISDIR) $(BINDIR)/$(OUTFILE)
	@echo "Packaging $@"
	cp $(BINDIR)/$(OUTFILE) $@

clean:
	@echo "Cleaning ...."
	$(RM) $(BINDIR)/*.exe $(BINDIR)/*.uf2 $(SRCDIR)/multirom.h $(SRCDIR)/menu.h $(SRCDIR)/nextor.h $(SRCDIR)/wifibios.h $(SRCDIR)/esp8266p_rom.h $(SRCDIR)/fmpac_bios.h $(SRCDIR)/sfg_bios.h $(BINDIR)/explorer.rom $(BINDIR)/explorer_payload.bin
	$(RM) $(DISDIR)/*
