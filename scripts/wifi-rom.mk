# Optional WiFi ROM assembly; normal host builds use the distributed ROM.
SJASMPLUS ?= sjasmplus
.PHONY: all clean
all:
	mkdir -p build
	$(SJASMPLUS) --raw="build/$(ROM)" --lst="build/$(ROM).lst" "$(ASM_SOURCE)"
clean:
	rm -f "build/$(ROM)" "build/$(ROM).lst"
