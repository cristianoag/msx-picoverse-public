# Shared project entry points. The default uses bundled firmware assets.
PYTHON ?= python3
JOBS ?= 4
# Each Pico build re-embeds its payload into the same native tool. Keep project
# targets ordered; CMake still compiles each component with JOBS workers.
.NOTPARALLEL:
.PHONY: all tool firmware pico msx bios wifi-config rebuild clean $(PICO_COMPONENTS)
all: tool
tool:
	$(MAKE) -C tool $(if $(VERSION),VERSION="$(VERSION)",)
firmware pico:
	$(PYTHON) ../../../scripts/pico-build.py $(PROJECT) --jobs $(JOBS)
$(PICO_COMPONENTS):
	$(PYTHON) ../../../scripts/pico-build.py 2040-$@ --jobs $(JOBS)
msx:
	$(MAKE) -C msx $(if $(VERSION),VERSION="$(VERSION)",)
bios:
	$(MAKE) -C ../wifi/bios
wifi-config:
	$(MAKE) -C ../wifi/config
rebuild: firmware $(PICO_COMPONENTS)
	$(MAKE) -C tool $(if $(VERSION),VERSION="$(VERSION)",)
clean:
	$(MAKE) -C tool clean
