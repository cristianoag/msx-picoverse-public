# Portable entry points. Native tools need Python 3 and a C compiler only.
PYTHON ?= python3
PROJECTS ?=
PICO_TARGET ?= 2040-midipac
JOBS ?= 4

.PHONY: all tools firmware release pico test clean help
all: tools
tools:
	$(PYTHON) scripts/build.py tools $(PROJECTS)
firmware: tools
	$(PYTHON) scripts/release.py firmware
release: tools
	$(PYTHON) scripts/release.py package
pico:
	$(PYTHON) scripts/pico-build.py $(PICO_TARGET) --jobs $(JOBS)
test:
	$(PYTHON) -m unittest discover -s tests -v
clean:
	$(PYTHON) scripts/build.py clean $(PROJECTS)
help:
	@echo "make tools [PROJECTS='2040-loadrom 2350-explorer'] - native UF2 builders"
	@echo "make firmware - generate system UF2 images in build/release/firmware"
	@echo "make release - package native tools and system images with checksums"
	@echo "make pico PICO_TARGET=2040-midipac - rebuild one Pico component from source"
	@echo "make test - run build and UF2 validation tests"
