# Host test for the PicoVerse Audio Engine.
#   make FATFS=<no-OS-FatFS-SD-SDIO-SPI-RPi-Pico>/src MP3=<picomp3lib>/src ASSETS=<test audio dir> run
.DEFAULT_GOAL := host_pva
FATFS  ?= ../../../lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src
MP3    ?= ../../../lib/picomp3lib/src
ASSETS ?= assets

CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -DPVA_HOST \
         -I.. -Ihostinc -I$(FATFS)/include -I$(FATFS)/ff15/source -I$(MP3)

PVA    = ../pva_window.c ../pva_engine.c ../pva_decode.c ../pva_net.c ../pva_esp.c ../pva_dir.c ../pva_eq.c ../pva_viz.c ../pva_json.c ../pva_web.c ../pva_cfg.c ../pva_clock.c ../pva_news.c ../pva_news_cat.c
FATSRC = $(FATFS)/ff15/source/ff.c $(FATFS)/ff15/source/ffunicode.c $(FATFS)/ff15/source/ffsystem.c
MP3SRC = $(filter-out $(MP3)/buffers.c,$(wildcard $(MP3)/*.c))

# same FatFS configuration as the firmware, plus f_chmod() for building the test image
hostinc/ffconf.h: $(FATFS)/include/ffconf.h
	mkdir -p hostinc
	sed 's/^#define FF_USE_CHMOD.*/#define FF_USE_CHMOD 1/' $< > $@

buffers.o: $(MP3)/buffers.c hostinc/ffconf.h
	$(CC) $(CFLAGS) -w -DMPDEC_ALLOCATOR=pva_mp3_alloc -include ../pva_mp3_alloc.h -c $< -o $@

host_pva: hostinc/ffconf.h host_pva.c host_hal.c host_esp.c buffers.o $(PVA) ../pva_proto.h ../pva_internal.h
	$(CC) $(CFLAGS) -o $@ host_pva.c host_hal.c host_esp.c buffers.o $(PVA) $(FATSRC) $(MP3SRC) -w -lm

run: host_pva
	./host_pva $(ASSETS)

clean:
	rm -rf host_pva buffers.o pva_test.img hostinc
