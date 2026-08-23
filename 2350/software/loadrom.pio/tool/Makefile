######################################################################
# MSX PICOVERSE PROJECT                                               
# (c) 2025 Cristiano Goncalves                                        
# The Retro Hacker                                                    
#                                                                      
# Makefile - build script for the MSX PICOVERSE 2350 loadROM tool    
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
CCFLAGS := -g

# Application metadata
VERSION ?= v2.74

# Project files
SOURCES := loadrom.c
OUTFILE := loadrom.exe

# Firmware assets (generated header embeds the Pico firmware image)
PICOBIN   := ../pico/loadrom/dist/loadrom.bin
PICOBIN_H := $(SRCDIR)/loadrom.h
NEXTOR_SUNRISE := ../resources/Nextor-2.1.4.SunriseIDE.MasterOnly.ROM
NEXTOR_SUNRISE_H := $(SRCDIR)/nextor_sunrise.h
ESP8266P_ROM := ../../wifi/bios/ESP8266P.rom
ESP8266P_ROM_H := $(SRCDIR)/esp8266p_rom.h
FMPAC_BIOS := ../fmpac/FMPCCMFC.BIN
FMPAC_BIOS_H := $(SRCDIR)/fmpac_bios.h
OPL4BIN   := ../pico/opl4/dist/opl4.bin
OPL4BIN_H := $(SRCDIR)/opl4_fw.h
OPL4BIN22K   := ../pico/opl4/dist/opl4_22k.bin
OPL4BIN22K_H := $(SRCDIR)/opl4_fw_22k.h
YRW801_ROM := ../resources/YRW801-M\ -\ Yamaha\ -\ 1993.rom
YRW801_ROM_H := $(SRCDIR)/yrw801_rom.h
MSXAUDIOBIN   := ../pico/msxaudio/dist/msxaudio.bin
MSXAUDIOBIN_H := $(SRCDIR)/msxaudio_fw.h
MSXAUDIO_BIOS := ../resources/MSXAUDIO_NMS1205.ROM
MSXAUDIO_BIOS_H := $(SRCDIR)/msxaudio_bios.h

# Helpers
RM := rm -f

.PHONY: all compile package clean

all: clean compile package

compile: $(BINDIR)/$(OUTFILE)

package: $(DISDIR)/loadrom.exe

$(BINDIR)/$(OUTFILE): $(BINDIR) $(SRCDIR)/$(SOURCES) $(PICOBIN_H) $(PICOBIN) $(NEXTOR_SUNRISE_H) $(NEXTOR_SUNRISE) $(ESP8266P_ROM_H) $(ESP8266P_ROM) $(FMPAC_BIOS_H) $(FMPAC_BIOS) $(OPL4BIN_H) $(OPL4BIN) $(OPL4BIN22K_H) $(OPL4BIN22K) $(YRW801_ROM_H) $(YRW801_ROM) $(MSXAUDIOBIN_H) $(MSXAUDIOBIN) $(MSXAUDIO_BIOS_H) $(MSXAUDIO_BIOS)
	@echo "Compiling $@"
	$(CC) $(CCFLAGS) -DAPP_VERSION=\"$(VERSION)\" $(SRCDIR)/$(SOURCES) -o $@

$(PICOBIN_H): $(PICOBIN)
	@echo "Embedding Pico firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(NEXTOR_SUNRISE_H): $(NEXTOR_SUNRISE)
	@echo "Embedding Sunrise Nextor ROM into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(ESP8266P_ROM_H): $(ESP8266P_ROM)
	@echo "Embedding ESP8266P WiFi ROM into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(FMPAC_BIOS_H): $(FMPAC_BIOS)
	@echo "Embedding FM-PAC BIOS ROM into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(OPL4BIN_H): $(OPL4BIN)
	@echo "Embedding standalone OPL4 firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(OPL4BIN22K_H): $(OPL4BIN22K)
	@echo "Embedding standalone OPL4 22.05 kHz firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(YRW801_ROM_H): $(YRW801_ROM)
	@echo "Embedding YRW801-M OPL4 ROM into header"
	$(UTLDIR)/$(XXD) -i "$<" $@

$(MSXAUDIOBIN_H): $(MSXAUDIOBIN)
	@echo "Embedding standalone MSX-AUDIO firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(MSXAUDIO_BIOS_H): $(MSXAUDIO_BIOS)
	@echo "Embedding MSX-AUDIO BIOS ROM into header"
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
	$(RM) $(BINDIR)/*.exe $(BINDIR)/*.uf2 $(SRCDIR)/loadrom.h $(SRCDIR)/nextor_sunrise.h $(SRCDIR)/esp8266p_rom.h $(SRCDIR)/fmpac_bios.h $(SRCDIR)/opl4_fw.h $(SRCDIR)/opl4_fw_22k.h $(SRCDIR)/yrw801_rom.h $(SRCDIR)/msxaudio_fw.h $(SRCDIR)/msxaudio_bios.h
	$(RM) $(DISDIR)/*
