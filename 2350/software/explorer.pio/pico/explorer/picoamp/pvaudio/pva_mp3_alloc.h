// MSX PICOVERSE PROJECT
// pva_mp3_alloc.h - allocator hook for the Helix MP3 decoder (picomp3lib).
//
// Force-included into picomp3lib's buffers.c with -DMPDEC_ALLOCATOR=pva_mp3_alloc
// so the decoder state lives in the engine arena. The pool is reset before every
// MP3InitDecoder() call and hands out zeroed memory.

#ifndef PVA_MP3_ALLOC_H
#define PVA_MP3_ALLOC_H

#include <stddef.h>

void *pva_mp3_alloc(size_t size);

#endif
