######################################################################
# MSX PICOVERSE PROJECT                                               
# (c) 2025 Cristiano Goncalves                                        
# The Retro Hacker                                                    
#                                                                      
# Makefile - build script for the MSX PICOVERSE 2040 multiROM tool    
#                                                                      
# This Makefile compiles the multiROM utility, regenerates the embedded 
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
VERSION ?= v2.65

# Project files
SOURCES := multirom.c
OUTFILE := multirom.exe

# Firmware assets (generated header embeds the Pico firmware image)
PICOBIN   := ../pico/multirom/build/multirom.bin
PICOBIN_H := $(SRCDIR)/multirom.h
MSXMENU := ../msx/dist/menu.rom
MSXMENU_H := $(SRCDIR)/menu.h
NEXTOR_SUNRISE := ../nextor/kernel/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM
NEXTOR_SUNRISE_H := $(SRCDIR)/nextor_sunrise.h
# ESPPROM = ../wireless/dist/ESP8266P.ROM

# Helpers
RM := rm -f

.PHONY: all compile package clean

all: clean compile package

compile: $(BINDIR)/$(OUTFILE)

package: $(DISDIR)/multirom.exe


$(BINDIR)/$(OUTFILE): $(BINDIR) $(SRCDIR)/$(SOURCES) $(PICOBIN_H) $(PICOBIN) $(MSXMENU_H) $(MSXMENU) $(NEXTOR_SUNRISE_H) $(NEXTOR_SUNRISE)
	@echo "Compiling $@"
	$(CC) $(CCFLAGS) -DAPP_VERSION=\"$(VERSION)\" $(SRCDIR)/$(SOURCES) -o $@

$(PICOBIN_H): $(PICOBIN)
	@echo "Embedding Pico firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(MSXMENU_H): $(MSXMENU)
	@echo "Embedding MSX menu ROM into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(NEXTOR_SUNRISE_H): $(NEXTOR_SUNRISE)
	@echo "Embedding Sunrise IDE Nextor ROM into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(BINDIR):
	@mkdir $@

$(DISDIR):
	@mkdir $@

$(DISDIR)/multirom.exe: $(DISDIR) $(BINDIR)/$(OUTFILE)
	@echo "Packaging $@"
	cp $(BINDIR)/$(OUTFILE) $@

clean:
	@echo "Cleaning ...."
	$(RM) $(BINDIR)/*.exe $(BINDIR)/*.uf2 $(SRCDIR)/multirom.h $(SRCDIR)/menu.h $(SRCDIR)/nextor_sunrise.h $(BINDIR)/multirom.rom $(BINDIR)/multirom_payload.bin
	$(RM) $(DISDIR)/*
