# Optional Z80 menu build. Supply a compatible Fusion-C installation.
PYTHON ?= python3
SDCC ?= sdcc
FUSION_DIR ?= ../../../../.deps/fusion-c
FUSION_HEADER ?= $(FUSION_DIR)/header
FUSION_CRT ?= $(FUSION_DIR)/include/crt0_MSX32k_ROM4000.rel
FUSION_LIB ?= $(FUSION_DIR)/lib/fusion_min_printf.lib
VERSION ?= $(DEFAULT_VERSION)
Z80FLAGS ?= -mz80 --code-loc 0x4050 --data-loc 0xC000 --disable-warning 196 --opt-code-size --no-std-crt0
RELS := $(SOURCES:%.c=build/%.rel)
HEADERS := $(wildcard src/*.h)

.PHONY: all compile package clean check-fusion
all: compile
compile: build/menu.rom
package: dist/menu.rom
check-fusion:
	@test -f "$(FUSION_CRT)" -a -f "$(FUSION_LIB)" -a -d "$(FUSION_HEADER)" || { echo "Set FUSION_DIR (or FUSION_HEADER/FUSION_CRT/FUSION_LIB) to a compatible Fusion-C checkout. See docs/BUILDING.md."; exit 1; }
build dist:
	mkdir -p "$@"
build/%.rel: src/%.c $(HEADERS) | build check-fusion
	$(SDCC) $(Z80FLAGS) -D$(VERSION_DEFINE)=\"$(VERSION)\" -I "$(FUSION_HEADER)" -c "$<" -o "$@"
build/menu.ihx: $(RELS) | check-fusion
	$(SDCC) $(Z80FLAGS) "$(FUSION_CRT)" $(RELS) "$(FUSION_LIB)" -o "$@"
build/menu.rom: build/menu.ihx ../../../../scripts/ihx2bin.py
	$(PYTHON) ../../../../scripts/ihx2bin.py "$<" "$@"
dist/menu.rom: build/menu.rom | dist
	cp "$<" "$@"
clean:
	rm -f build/*.asm build/*.ihx build/*.lk build/*.lst build/*.map build/*.noi build/*.rel build/*.sym build/*.rom
