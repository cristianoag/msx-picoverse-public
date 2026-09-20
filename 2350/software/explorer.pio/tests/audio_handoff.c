#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu2413.h"
#include "handoff_allocator.h"

typedef union block {
    max_align_t alignment;
    struct { size_t size; bool used; union block *next; } data;
} block_t;
static union { max_align_t alignment; uint8_t bytes[47880]; } heap;

static void heap_reset(void) {
    memset(&heap, 0, sizeof(heap));
    ((block_t *)heap.bytes)->data.size = sizeof(heap.bytes) - sizeof(block_t);
}
void *handoff_malloc(size_t size) {
    size = (size + sizeof(max_align_t) - 1) / sizeof(max_align_t) * sizeof(max_align_t);
    for (block_t *b = (block_t *)heap.bytes; b; b = b->data.next) {
        if (b->data.used || b->data.size < size) continue;
        if (b->data.size >= size + sizeof(block_t) + sizeof(max_align_t)) {
            block_t *rest = (block_t *)((uint8_t *)(b + 1) + size);
            rest->data.size = b->data.size - size - sizeof(block_t);
            rest->data.used = false;
            rest->data.next = b->data.next;
            b->data.next = rest;
            b->data.size = size;
        }
        b->data.used = true;
        return b + 1;
    }
    return NULL;
}
void *handoff_calloc(size_t count, size_t size) {
    void *ptr = handoff_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}
void handoff_free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)ptr - 1;
    assert(b->data.used);
    b->data.used = false;
    for (b = (block_t *)heap.bytes; b && b->data.next;) {
        block_t *next = b->data.next;
        if (!b->data.used && !next->data.used) {
            b->data.size += sizeof(block_t) + next->data.size;
            b->data.next = next->data.next;
        } else b = next;
    }
}

struct mem_buffer { size_t size; uint8_t *bytes; };
struct audio_format { unsigned sample_freq; };
struct audio_buffer_format { struct audio_format *format; unsigned sample_stride; };
struct audio_buffer {
    struct mem_buffer *buffer;
    struct audio_buffer_format *format;
    unsigned sample_count, max_sample_count;
    struct audio_buffer *next;
};
struct audio_buffer_pool {
    struct audio_buffer *free_list, *prepared_list;
    struct audio_format *format;
    void *connection;
};
static struct audio_buffer_pool *rom_audio_handoff_pool;
static bool i2s_enabled;
static unsigned disable_count;
static void audio_i2s_set_enabled(bool enabled) {
    assert(!enabled);
    i2s_enabled = enabled;
    disable_count++;
}
static struct audio_buffer *pop(struct audio_buffer **head) {
    assert(!i2s_enabled);
    struct audio_buffer *b = *head;
    if (b) { *head = b->next; b->next = NULL; }
    return b;
}
static struct audio_buffer *get_full_audio_buffer(struct audio_buffer_pool *p, bool block) {
    assert(!block); return pop(&p->prepared_list);
}
static struct audio_buffer *get_free_audio_buffer(struct audio_buffer_pool *p, bool block) {
    assert(!block); return pop(&p->free_list);
}
static void queue_free_audio_buffer(struct audio_buffer_pool *p, struct audio_buffer *b) {
    assert(!i2s_enabled && !b->next);
    b->next = p->free_list;
    p->free_list = b;
}
static struct mem_buffer *pico_buffer_alloc(size_t size) {
    struct mem_buffer *b = handoff_malloc(sizeof(*b));
    if (!b) return NULL;
    b->bytes = handoff_calloc(1, size);
    if (!b->bytes) { handoff_free(b); return NULL; }
    b->size = size;
    return b;
}
#define debug_trace(...) ((void)0)
#define panic(message) do { fprintf(stderr, "%s\n", message); abort(); } while (0)
#define free handoff_free
#include "handoff-production.h"
#undef free

static void test_transition(unsigned producer_count, unsigned held, bool expect_exhaustion) {
    heap_reset();
    assert(handoff_malloc(7000)); // Existing menu/I2S allocations plus host bookkeeping.
    struct audio_format format = {44100};
    struct audio_buffer_format buffer_format = {&format, 4};
    struct audio_buffer buffers[8] = {0};
    struct audio_buffer_pool pool = {.format = &format, .connection = &pool};
    i2s_enabled = false;
    assert(producer_count <= 8 && held < producer_count);
    for (unsigned i = 0; i < producer_count; i++) {
        buffers[i].format = &buffer_format;
        buffers[i].buffer = pico_buffer_alloc(1152 * 4);
        assert(buffers[i].buffer);
        buffers[i].max_sample_count = buffers[i].sample_count = 1152;
        if (i < held) continue;
        if (i & 1) queue_free_audio_buffer(&pool, &buffers[i]);
        else { buffers[i].next = pool.prepared_list; pool.prepared_list = &buffers[i]; }
    }
    // Keep the original eight-buffer failure as a regression case, and also
    // exercise the current production buffer count with the real emulator.
    OPLL *opll = OPLL_new(3579545, 49716);
    if (expect_exhaustion) assert(!opll);
    else {
        assert(opll);
        OPLL_delete(opll);
    }
    uint8_t *held_bytes = buffers[0].buffer->bytes;
    rom_audio_handoff_pool = &pool;
    i2s_enabled = true;
    prepare_rom_audio_handoff_pool();
    assert(rom_audio_handoff_pool == &pool && pool.connection == &pool);
    assert(pool.format == &format && format.sample_freq == 44100);
    assert(!i2s_enabled && !pool.prepared_list);
    unsigned count = 0;
    for (struct audio_buffer *b = pool.free_list; b; b = b->next) {
        assert(b->max_sample_count == 256 && b->sample_count == 0);
        assert(b->buffer->size == 256 * 4);
        memset(b->buffer->bytes, 0x5A, 256 * 4);
        count++;
    }
    assert(count == producer_count - held);
    if (held) {
        assert(buffers[0].buffer->bytes == held_bytes);
        assert(buffers[0].buffer->size == 1152 * 4 && buffers[0].max_sample_count == 1152);
    }
    opll = OPLL_new(3579545, 49716);
    assert(opll && !opll->conv);
    OPLL_writeReg(opll, 0x30, 0x10);
    OPLL_writeReg(opll, 0x10, 0x80);
    OPLL_writeReg(opll, 0x20, 0x15);
    bool audible = false;
    for (unsigned i = 0; i < 8192; i++) audible |= OPLL_calc(opll) != 0;
    assert(audible);
    OPLL_delete(opll);
    prepare_rom_audio_handoff_pool();
    assert(!pool.prepared_list);
}

int main(void) {
    rom_audio_handoff_pool = NULL;
    prepare_rom_audio_handoff_pool();
    assert(disable_count == 0);
    test_transition(8, 0, true);
    test_transition(8, 1, true);
    test_transition(MP3_I2S_BUFFER_COUNT, 0, false);
    test_transition(MP3_I2S_BUFFER_COUNT, 1, false);
    puts("PASS: original eight-buffer exhaustion; current MP3 pool and compacted FM handoff preserve connection/held buffers");
    return 0;
}
