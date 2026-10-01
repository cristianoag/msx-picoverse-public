######################################################################
# MSX PICOVERSE 2040 multiROM - openMSX add-on                        #
#                                                                    #
# Builds the placeholder ROM of the "PicoVerse 2040 MultiROM" openMSX #
# extension (share/extensions/PicoVerse_2040). Also run by the        #
# aggregate multirom.pio Makefile, which passes VERSION.              #
######################################################################

ASM    = sdasz80
LINK   = sdldz80
HEXBIN = hex2bin

SRCDIR = src
BINDIR = build
EXTDIR = share/extensions/PicoVerse_2040
OUTROM = $(EXTDIR)/picoverse2040.rom
SCRIPT = share/scripts/picoverse2040.tcl

# Expected script version; empty skips the check (standalone builds)
VERSION ?=

.PHONY: all check-version clean

all: check-version $(OUTROM)

# The Tcl script is not generated, so make sure its version string follows
# the multirom.pio version.
check-version:
ifneq ($(VERSION),)
	@grep -q '^variable version "$(VERSION)"' $(SCRIPT) || \
		(echo "$(SCRIPT): 'variable version' does not match $(VERSION)"; exit 1)
endif

$(BINDIR)/placeholder.rel: $(SRCDIR)/placeholder.asm
	@mkdir -p $(BINDIR)
	$(ASM) -o $@ $<

$(BINDIR)/placeholder.ihx: $(BINDIR)/placeholder.rel
	$(LINK) -i $@ $<

$(OUTROM): $(BINDIR)/placeholder.ihx
	$(HEXBIN) -e rom -s 0x4000 -l 0x4000 $<
	cp $(BINDIR)/placeholder.rom $@

clean:
	rm -rf $(BINDIR)
