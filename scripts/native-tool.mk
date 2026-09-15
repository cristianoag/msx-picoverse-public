# Shared native build entry point; caller sets PROJECT.
PYTHON ?= python3
ROOT := ../../../..
.NOTPARALLEL:
.PHONY: all compile package clean
all: compile
compile:
	$(PYTHON) "$(ROOT)/scripts/build.py" tools $(PROJECT) $(if $(VERSION),--version "$(VERSION)",)
# Native binaries are written next to this Makefile, ready to run or package.
package: compile
clean:
	$(PYTHON) "$(ROOT)/scripts/build.py" clean $(PROJECT)
