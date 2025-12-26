CC = gcc

SRCDIR = src
BINDIR = build
DISDIR = dist

VERBOSE = --verbose
CCFLAGS = -g 

SOURCES = loadmp3.c
OUTFILE = loadmp3.exe

PICOBIN = ../pico/loadmp3/dist/loadmp3.bin

all: clean compile package

compile: $(BINDIR)/$(OUTFILE)

$(BINDIR)/$(OUTFILE): $(SRCDIR)/$(SOURCES)
	@echo "Compiling $@"
	$(CC) $(CCFLAGS) $< -o $@

package:
	@echo "Packaging..."
	cp $(BINDIR)/$(OUTFILE) $(DISDIR)/$(OUTFILE)
	cp $(PICOBIN) $(BINDIR)/loadmp3.bin 
	cp $(PICOBIN) $(DISDIR)/loadmp3.bin 

clean:
		@echo "Cleaning ...."
		rm -f $(BINDIR)/*.exe $(BINDIR)/loadmp3.cfg $(BINDIR)/loadmp3.bin $(BINDIR)/loadmp3.cmb $(BINDIR)/loadmp3.uf2
		rm -f $(DISDIR)/*.*

