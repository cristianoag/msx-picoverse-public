######################################################################
# MSX PICOVERSE 2040 PROJECT                                               
# (c) 2025 Cristiano Goncalves                                        
# The Retro Hacker                                                    
# 
# Nextor driver build makefile
# Generates the custom Nextor ROM with the MultiROM driver.                                            
######################################################################

# Toolchain configuration
CPP = sdcc
N80 = n80.exe
SJASM = tools/sjasmplus.exe
ASM = sdasz80
MKNEXROM = mknexrom.exe
HEX2BIN = hex2bin

# Debug control: set DEBUG=1 on the make command line to enable debug build
# Example: `make DEBUG=1`
DEBUG ?= 0

ifeq ($(DEBUG),1)
SDCC_FLAGS = -mz80 -DDEBUG
else
SDCC_FLAGS = -mz80 -DNODEBUG
endif

# Build flags

HEX2BIN_FLAGS = -l 3fd0 -e rom

# Project metadata
VERSION ?= 1.01
SDCC_DEFINES = -DNEXTOR_DRIVER_VERSION=\"$(VERSION)\"

# Helpers
RM = rm -f
CP = cp
MKDIR = mkdir -p

# Directory layout
BUILD_DIR = build
DIST_DIR = dist
SRC_DIR = src
TOOLS_DIR = tools
KERNEL_DIR = kernel

# Project files
NEXTOR_BASE = $(KERNEL_DIR)/Nextor-2.1.4.base.dat
NEXTOR_ROM =  $(BUILD_DIR)/nextor.rom
NEXTOR_MAPPER = $(KERNEL_DIR)/Mapper.ASCII16.bin
IHX = $(BUILD_DIR)/driver.ihx
BIN = $(BUILD_DIR)/driver.rom

# Memory layout
ADDR_CODE = 0x4220
ADDR_DATA = 0xc800

.PHONY: all clean package

all: clean $(NEXTOR_ROM) package

$(BUILD_DIR):
	@mkdir $@

$(DIST_DIR):
	@mkdir $@

$(BUILD_DIR)/driver.rel: $(SRC_DIR)/driver.c $(BUILD_DIR)
	$(CPP) $(SDCC_FLAGS) $(SDCC_DEFINES) -c -o $@ $<

$(BUILD_DIR)/hal.rel: $(SRC_DIR)/hal.c $(BUILD_DIR)
	$(CPP) $(SDCC_FLAGS) $(SDCC_DEFINES) -c -o $@ $<

$(BUILD_DIR)/msxromcrt0.rel: $(SRC_DIR)/msxromcrt0.s $(BUILD_DIR)
	$(ASM) -o $@ $<

$(BUILD_DIR)/extra1k.dat: $(SRC_DIR)/extra1k.asm $(BUILD_DIR)
	$(SJASM) --fullpath --raw=$@ --sym=$(basename $@).sym --lst=$(basename $@).lst $<

$(IHX): $(BUILD_DIR)/msxromcrt0.rel $(BUILD_DIR)/hal.rel $(BUILD_DIR)/driver.rel
	$(CPP) $(SDCC_FLAGS) --code-loc $(ADDR_CODE) --data-loc $(ADDR_DATA) --no-std-crt0 -o $@ $^

$(BIN): $(IHX)
	@echo "Building driver ROM..."
	$(HEX2BIN) $(HEX2BIN_FLAGS) $<

$(NEXTOR_ROM): $(BIN) $(BUILD_DIR)/extra1k.dat
	@echo "Building Nextor ROM..."
	$(TOOLS_DIR)/$(MKNEXROM) $(NEXTOR_BASE) $@ /d:$(BIN) /e:$(BUILD_DIR)/extra1k.dat

package: $(NEXTOR_ROM) $(DIST_DIR)
	@echo "Creating package..."
	@$(CP) $(NEXTOR_ROM) $(DIST_DIR)/nextor.rom

clean:
	@echo "Cleaning..."
	@$(RM) $(BUILD_DIR)/driver.* $(BUILD_DIR)/nextor.rom $(BUILD_DIR)/msxromcrt0.rel $(BUILD_DIR)/hal.*
	@$(RM) $(DIST_DIR)/nextor.rom 
	@$(RM) $(IHX) $(BIN)