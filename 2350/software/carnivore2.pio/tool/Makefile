######################################################################
# MSX PICOVERSE PROJECT
# (c) 2026 Cristiano Goncalves
# The Retro Hacker
#
# Makefile - build script for the MSX PICOVERSE 2350 Carnivore2 tool
#
# This Makefile compiles the Carnivore2 UF2 creator, regenerating the
# embedded Raspberry Pi Pico firmware header, and packages the executable.
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
CCFLAGS := -g

# Application metadata
VERSION ?= v1.1

# Project files
SOURCES := carnivore2.c
OUTFILE := carnivore2.exe

# Firmware asset (generated header embeds the Pico firmware image)
PICOBIN   := ../pico/carnivore2/dist/carnivore2.bin
PICOBIN_H := $(SRCDIR)/carnivore2.h

# Helpers
RM := rm -f

.PHONY: all compile package clean

all: clean compile package

compile: $(BINDIR)/$(OUTFILE)

package: $(DISDIR)/$(OUTFILE)

$(BINDIR)/$(OUTFILE): $(BINDIR) $(SRCDIR)/$(SOURCES) $(PICOBIN_H) $(PICOBIN)
	@echo "Compiling $@"
	$(CC) $(CCFLAGS) -DAPP_VERSION=\"$(VERSION)\" $(SRCDIR)/$(SOURCES) -o $@

$(PICOBIN_H): $(PICOBIN)
	@echo "Embedding Pico firmware into header"
	$(UTLDIR)/$(XXD) -i $< $@

$(BINDIR):
	@mkdir -p $(BINDIR)

$(DISDIR):
	@mkdir -p $(DISDIR)

$(DISDIR)/$(OUTFILE): $(DISDIR) $(BINDIR)/$(OUTFILE)
	@echo "Packaging $@"
	@cp $(BINDIR)/$(OUTFILE) $(DISDIR)/$(OUTFILE)

clean:
	@echo "Cleaning tool build artifacts"
	$(RM) $(BINDIR)/$(OUTFILE)
	$(RM) $(DISDIR)/$(OUTFILE)
	$(RM) $(PICOBIN_H)
