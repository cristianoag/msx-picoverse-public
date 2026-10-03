######################################################################
# MSX PICOVERSE PROJECT                                               
# (c) 2026 Cristiano Goncalves                                        
# The Retro Hacker                                                    
#                                                                      
# Makefile - build script for the MSX PICOVERSE 2040 loadROM tool    
#                                                                      
# This Makefile compiles the loadROM utility, regenerates the embedded 
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

# Debug control: set DEBUG=1 when invoking make to enable debug symbols and -DDEBUG
# Example: `make DEBUG=1`
DEBUG ?= 0

ifeq ($(DEBUG),1)
CCFLAGS := -g -DDEBUG
else
CCFLAGS := -g
endif

# Application metadata
VERSION ?= v2.63

# Project files
SOURCES := loadrom.c 
OUTFILE := loadrom.exe

# Firmware assets (generated header embeds the Pico firmware image)
PICOBIN   := ../pico/loadrom/dist/loadrom.bin
PICOBIN_H := $(SRCDIR)/loadrom.h
NEXTOR_SUNRISE := ../nextor/kernel/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM
#NEXTOR_SUNRISE := ../nextor/kernel/Nextor-2.1.4.SunriseIDE.ROM
NEXTOR_SUNRISE_H := $(SRCDIR)/nextor_sunrise.h
KEYBOARDBIN   := ../pico/keyboard/dist/keyboard.bin
KEYBOARDBIN_H := $(SRCDIR)/keyboard.h
MIDIBIN       := ../pico/midi/dist/midi.bin
MIDIBIN_H     := $(SRCDIR)/midi_fw.h
MIDIPACBIN    := ../pico/midipac/dist/midipac.bin
MIDIPACBIN_H  := $(SRCDIR)/midipac_fw.h

# Helpers
RM := rm -f

.PHONY: all compile package clean

all: clean compile package

compile: $(BINDIR)/$(OUTFILE)

package: $(DISDIR)/loadrom.exe

$(BINDIR)/$(OUTFILE): $(BINDIR) $(SRCDIR)/$(SOURCES) $(PICOBIN_H) $(PICOBIN) $(NEXTOR_SUNRISE_H) $(NEXTOR_SUNRISE) $(KEYBOARDBIN_H) $(KEYBOARDBIN) $(MIDIBIN_H) $(MIDIBIN) $(MIDIPACBIN_H) $(MIDIPACBIN)
	@echo "Compiling $@"
	$(CC) $(CCFLAGS) -DAPP_VERSION=\"$(VERSION)\" $(SRCDIR)/$(SOURCES) -o $@

$(PICOBIN_H): $(PICOBIN)
	@echo "Embedding Pico firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(NEXTOR_SUNRISE_H): $(NEXTOR_SUNRISE)
	@echo "Embedding Sunrise Nextor ROM into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(KEYBOARDBIN_H): $(KEYBOARDBIN)
	@echo "Embedding Keyboard firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(MIDIBIN_H): $(MIDIBIN)
	@echo "Embedding MIDI firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(MIDIPACBIN_H): $(MIDIPACBIN)
	@echo "Embedding MIDI-PAC firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(BINDIR):
	@mkdir $@

$(DISDIR):
	@mkdir $@

$(DISDIR)/loadrom.exe: $(DISDIR) $(BINDIR)/$(OUTFILE)
	@echo "Packaging $@"
	cp $(BINDIR)/$(OUTFILE) $@

clean:
	@echo "Cleaning ...."
	$(RM) $(BINDIR)/*.exe $(BINDIR)/*.uf2 $(SRCDIR)/loadrom.h $(SRCDIR)/nextor_sunrise.h $(SRCDIR)/keyboard.h $(SRCDIR)/midi_fw.h $(SRCDIR)/midipac_fw.h
	$(RM) $(DISDIR)/*

