#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OPL4_PORT_FM_REG   0xC4u
#define OPL4_PORT_FM_DATA  0xC5u
#define OPL4_PORT_FM2_REG  0xC6u
#define OPL4_PORT_FM2_DATA 0xC7u
#define OPL4_PORT_PCM_REG  0x7Eu
#define OPL4_PORT_PCM_DATA 0x7Fu

static bool opl4_ready = true;

// The chip and the memory window are not under test: record what reaches them.
static struct { uint8_t offset, data; } bus_writes[16];
static unsigned bus_write_count;
static uint8_t bus_read_offset = 0xFFu;

static void opl4_bus_write(uint8_t offset, uint8_t data) {
    assert(bus_write_count < 16);
    bus_writes[bus_write_count].offset = offset;
    bus_writes[bus_write_count].data = data;
    bus_write_count++;
}

static uint8_t opl4_bus_read(uint8_t offset) {
    bus_read_offset = offset;
    return (uint8_t)(0x40u + offset);
}

// Scripted capture FIFOs. A write is "late" when it is captured after the read
// address was dequeued, the window between the two drains of opl4_service_io().
static struct { uint16_t addr; uint8_t data; bool late; } writes[8];
static unsigned write_count, write_next;
static uint16_t reads[4];
static unsigned read_count, read_next;
static bool read_dequeued;
static struct { bool in_window; uint8_t data; } responses[4];
static unsigned response_count;

static bool pio_try_get_io_write(uint16_t *addr, uint8_t *data) {
    if (write_next >= write_count)
        return false;
    if (writes[write_next].late && !read_dequeued)
        return false;
    *addr = writes[write_next].addr;
    *data = writes[write_next].data;
    write_next++;
    return true;
}

static bool pio_try_get_io_read(uint16_t *addr) {
    if (read_next >= read_count)
        return false;
    *addr = reads[read_next++];
    read_dequeued = true;
    return true;
}

static void io_read_respond(bool in_window, uint8_t data) {
    assert(response_count < 4);
    responses[response_count].in_window = in_window;
    responses[response_count].data = data;
    response_count++;
}

#include "opl4-production.h"

static void reset(void) {
    memset(bus_writes, 0, sizeof(bus_writes));
    memset(writes, 0, sizeof(writes));
    memset(reads, 0, sizeof(reads));
    memset(responses, 0, sizeof(responses));
    bus_write_count = write_count = write_next = read_count = read_next = response_count = 0;
    read_dequeued = false;
    bus_read_offset = 0xFFu;
    opl4_ready = true;
}

int main(void) {
    assert(opl4_port_offset(0xC4) == 0 && opl4_port_offset(0xC5) == 1 && opl4_port_offset(0xC6) == 2 &&
           opl4_port_offset(0xC7) == 3 && opl4_port_offset(0x7E) == 4 && opl4_port_offset(0x7F) == 5);
    assert(opl4_port_offset(0x7C) == 0xFF && opl4_port_offset(0x98) == 0xFF && opl4_port_offset(0xC3) == 0xFF);

    // A write that lands between the two drains must not change the port the
    // read is answered for: status read (0xC4), late write to the wave register
    // select (0x7E). The write is applied first, the read still answers 0xC4.
    reset();
    reads[0] = 0x00C4; read_count = 1;
    writes[0].addr = 0x007E; writes[0].data = 0x12; writes[0].late = true; write_count = 1;
    opl4_service_io();
    assert(bus_write_count == 1 && bus_writes[0].offset == 4 && bus_writes[0].data == 0x12);
    assert(bus_read_offset == 0);
    assert(response_count == 1 && responses[0].in_window && responses[0].data == 0x40);

    // The same with the late write to an unrelated port: the OPL4 read is still
    // answered, and the foreign write is ignored.
    reset();
    reads[0] = 0x007F; read_count = 1;
    writes[0].addr = 0x0098; writes[0].data = 0x55; writes[0].late = true; write_count = 1;
    opl4_service_io();
    assert(bus_write_count == 0 && bus_read_offset == 5);
    assert(response_count == 1 && responses[0].in_window && responses[0].data == 0x45);

    // A read of a foreign port is released undriven, even after a late OPL4 write.
    reset();
    reads[0] = 0x0099; read_count = 1;
    writes[0].addr = 0x00C5; writes[0].data = 0x77; writes[0].late = true; write_count = 1;
    opl4_service_io();
    assert(bus_write_count == 1 && bus_writes[0].offset == 1 && bus_read_offset == 0xFF);
    assert(response_count == 1 && !responses[0].in_window);

    // Writes captured before the read are applied in order, then the read answers.
    reset();
    writes[0].addr = 0x00C4; writes[0].data = 0x20;
    writes[1].addr = 0x00C5; writes[1].data = 0x01;
    write_count = 2;
    reads[0] = 0x00C4; read_count = 1;
    opl4_service_io();
    assert(bus_write_count == 2 && bus_writes[0].offset == 0 && bus_writes[1].offset == 1);
    assert(bus_read_offset == 0 && response_count == 1 && responses[0].data == 0x40);

    // Not ready (chip missing): nothing is applied and every read is released.
    reset();
    opl4_ready = false;
    reads[0] = 0x00C4; read_count = 1;
    writes[0].addr = 0x00C5; writes[0].data = 0x01; write_count = 1;
    opl4_service_io();
    assert(bus_write_count == 0 && bus_read_offset == 0xFF);
    assert(response_count == 1 && !responses[0].in_window);

    printf("PASS: OPL4 I/O service answers the read for its own port when a write lands between the drains\n");
    return 0;
}
