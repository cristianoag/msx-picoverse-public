######################################################################
# MSX PICOVERSE 2350 Explorer - openMSX add-on                       #
#                                                                    #
# Regenerates the ROM database of the "PicoVerse 2350 Explorer"      #
# openMSX extension from the firmware's pico/explorer/romdb.h, so     #
# both detect the same mappers, and checks the script version. Also  #
# run by the aggregate explorer.pio Makefile, which passes VERSION.  #
######################################################################

PYTHON ?= python

EXTDIR = share/extensions/PicoVerse_2350
ROMDB  = $(EXTDIR)/romdb.txt
ROMDB_SRC = ../pico/explorer/romdb.h
SCRIPT = share/scripts/picoverse2350.tcl

# Expected script version; empty skips the check (standalone builds)
VERSION ?=

.PHONY: all check-version clean

all: check-version $(ROMDB)

# The Tcl script is not generated, so make sure its version string follows
# the explorer.pio version.
check-version:
ifneq ($(VERSION),)
	@grep -q '^variable version "$(VERSION)"' $(SCRIPT) || \
		(echo "$(SCRIPT): 'variable version' does not match $(VERSION)"; exit 1)
endif

$(ROMDB): $(ROMDB_SRC) gen_romdb.py
	$(PYTHON) gen_romdb.py $(ROMDB_SRC) $@

# romdb.txt is kept in the repository (installers copy it as is).
clean:
