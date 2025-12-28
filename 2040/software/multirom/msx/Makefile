CC = sdcc
ASM = sdasz80
PLATFORM = -mz80
HEXBIN = hex2bin

FUSIONINC = /DevArea/libraries/fusion-c/include
FUSIONHEADER = /DevArea/libraries/fusion-c/header
FUSIONLIBDIR = /DevArea/libraries/fusion-c/lib
FUSIONLIB = $(FUSIONLIBDIR)/fusion_min_printf.lib

SRCDIR = src
BINDIR = build
DISDIR = dist

# See startup files for the correct ADDR_CODE and ADDR_DATA
# 16K ROM 4000H
#CRT0 = $(FUSIONINC)\crt0_MSX816kROM4000.rel
#ADDR_CODE = 0x4020
#ADDR_DATA = 0xC000
# 32K ROM 4000H
CRT0 = $(FUSIONINC)/crt0_MSX32k_ROM4000.rel
ADDR_CODE = 0x4050
ADDR_DATA = 0xC000

VERBOSE = --verbose
CCFLAGS = $(VERBOSE) --code-loc $(ADDR_CODE) --data-loc $(ADDR_DATA) --disable-warning 196 
DEBUGFLAGS =-g -DDEBUG
ifdef DEBUG
CCFLAGS += $(DEBUGFLAGS)
endif

# Project metadata
VERSION ?= v1.03
CCDEFS = -DMULTIROM_VERSION=\"$(VERSION)\"

SOURCES = menu.c
OUTFILE = menu.rom

all: clean compile package

compile: $(BINDIR)/$(OUTFILE)

$(BINDIR)/menu.ihx: $(SRCDIR)/$(SOURCES)
	@echo "Compiling $@"
	$(CC) $(CCFLAGS) $(CCDEFS) $(PLATFORM) --use-stdout --no-std-crt0 $(CRT0) $(FUSIONLIB) -I $(FUSIONHEADER) $< -o $@ 

$(BINDIR)/$(OUTFILE): $(BINDIR)/menu.ihx
	@echo "Building $(OUTFILE)..."
# 16K ROM 4000H
#	@$(HEXBIN) -e ROM -l 0x4000 $(BINDIR)/$(OUTFILE) $< 
# 32K ROM 4000H
	@$(HEXBIN) -e ROM -s 0x4000 -l 0x8000 $(BINDIR)/$(OUTFILE) $<

package:
	@echo "Packaging..."
	cp $(BINDIR)/$(OUTFILE) $(DISDIR)/$(OUTFILE)

clean:
		@echo "Cleaning ...."
		rm -f $(BINDIR)/*.asm $(BINDIR)/*.bin $(BINDIR)/*.cdb $(BINDIR)/*.ihx $(BINDIR)/*.lk $(BINDIR)/*.lst \
			$(BINDIR)/*.map $(BINDIR)/*.mem $(BINDIR)/*.omf $(BINDIR)/*.rst $(BINDIR)/*.rel $(BINDIR)/*.sym \
			$(BINDIR)/*.noi $(BINDIR)/*.hex $(BINDIR)/*.lnk $(BINDIR)/*.dep 
		rm -f $(BINDIR)/$(OUTFILE)
