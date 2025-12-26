// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// nextor.c - This is the Raspberry Pico firmware that will be used as a bridge between MSX NEXTOR and Mass Storage Devices (SD Card and USB).
//
// MSX NEXTOR bridge: the Pico speaks a simple control/data protocol while proxying SDCard/USB MSC.
// Control port writes choose an action, data port transfers payloads, and appropriate libraries perform
// the heavy lifting once the bus is idle. The state machine below keeps strict separation
// between MSX bus timing and asynchronous disk (sd and USB) transactions.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/


#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hw_config.h"
#include "multirom.h"
#include "nextor.h"


#define NEXTOR_STATUS_READY       0x00
#define NEXTOR_STATUS_BUSY        0x01
#define NEXTOR_STATUS_SENDING     0x02
#define NEXTOR_STATUS_ERROR       0xFF

#define NEXTOR_DATA_BUS_MASK      (0xFFu << DATA_PINS)


static inline void drive_data_bus(uint8_t value) {
    gpio_set_dir_out_masked(NEXTOR_DATA_BUS_MASK);
    gpio_put_masked(NEXTOR_DATA_BUS_MASK, (((uint32_t)value) << DATA_PINS) & NEXTOR_DATA_BUS_MASK);
}

static inline void release_data_bus(void) {
    gpio_set_dir_in_masked(NEXTOR_DATA_BUS_MASK);
}

// Waits for RD/WR and IORQ to deassert so we only service a single Z80 IO cycle per transaction.
static inline void wait_for_io_cycle_end(uint32_t signal_mask, uint32_t iorq_mask) {
    while (true) {
        uint32_t state = gpio_get_all();
        if ((state & signal_mask) && (state & iorq_mask)) {
            break;
        }
        tight_loop_contents();
    }
}

// Nextor on SD Card I/O handler function
// This function runs in core 1 and handles the Nextor SD Card I/O protocol
void __not_in_flash_func(nextor_sd_io)(){

    uint8_t  data_response_buffer[512]; // Buffer for data responses

    uint8_t ctr_val = NEXTOR_STATUS_READY; // Control/status register value returned on port 0x9E
    uint8_t out_val = 0;                   // Last data response for port 0x9F

    uint16_t data_to_send = 0;  // Bytes left to send to MSX
    uint16_t data_to_receive = 0; // Bytes left to receive from MSX
    uint16_t data_byte_index = 0; // Current index in the data buffer

    uint32_t block_address = 0; // Block address for read/write operations
    bool read_address = false; // Flag indicating if we are reading an address

    const BYTE pdrv = 0; // Physical drive number
    DSTATUS ds = STA_NOINIT; // Disk status (initialized to not initialized)

    memset(data_response_buffer, 0xFF, sizeof(data_response_buffer)); // Initialize buffer to 0xFF

    const uint32_t pin_mask_iorq = 1u << PIN_IORQ; // Mask for IORQ pin
    const uint32_t pin_mask_rd = 1u << PIN_RD; // Mask for RD pin
    const uint32_t pin_mask_wr = 1u << PIN_WR; // Mask for WR pin

    while (true) {

        uint32_t gpiostates = gpio_get_all(); // Read all GPIO states

        if (gpiostates & pin_mask_iorq) { // IORQ not active
            tight_loop_contents();
            continue;
        }

        bool wr = !(gpiostates & pin_mask_wr); // Write cycle (active low)
        bool rd = !(gpiostates & pin_mask_rd); // Read cycle (active low)

        if (wr == rd) { // Invalid state, wait for next cycle
            tight_loop_contents();
            continue;
        }

        uint8_t port = gpiostates & 0xFF; // Extract port address
        uint8_t busdata = (gpiostates >> DATA_PINS) & 0xFF; // Extract data bus value

        if (wr) { // Write transaction: the MSX is writing to the port.
            if (port == 0x9E) { // Port 0x9E (Control Write): Set the control register.
                switch (busdata) { // Command byte for the Nextor driver
                    case 0x01: // Initialize SD card
                        ctr_val = NEXTOR_STATUS_BUSY;
                        ds = disk_initialize(pdrv);
                        ctr_val = (ds & STA_NOINIT) ? NEXTOR_STATUS_ERROR : NEXTOR_STATUS_READY;
                        break;
                    case 0x03: // Manufacturer ID
                        ctr_val = NEXTOR_STATUS_BUSY;
                        if (!(ds & STA_NOINIT)) {
                            sd_card_t *sd_card = sd_get_by_num(0);
                            data_response_buffer[0] = (uint8_t)ext_bits16(sd_card->state.CID, 127, 120);
                            data_to_send = 1;
                            data_byte_index = 0;
                            ctr_val = NEXTOR_STATUS_READY;
                        } else {
                            ctr_val = NEXTOR_STATUS_ERROR;
                        }
                        break;
                    case 0x05: // Get capacity
                    {
                        ctr_val = NEXTOR_STATUS_BUSY;
                        if (!(ds & STA_NOINIT)) {
                            DWORD capacity = 0;
                            DRESULT dr = disk_ioctl(pdrv, GET_SECTOR_COUNT, &capacity);
                            if (dr == RES_OK) {
                                data_to_send = 4;
                                data_byte_index = 0;
                                memcpy(data_response_buffer, &capacity, 4);
                                ctr_val = NEXTOR_STATUS_READY;
                            } else {
                                data_to_send = 0;
                                ctr_val = NEXTOR_STATUS_ERROR;
                            }
                        } else {
                            ctr_val = NEXTOR_STATUS_ERROR;
                        }
                        break;
                    }
                    case 0x06: // Read block (first stage gets address, second stage reads block)
                        if (ds & STA_NOINIT) {
                            ctr_val = NEXTOR_STATUS_ERROR;
                            break;
                        }
                        if (!read_address) {
                            data_to_receive = 4;
                            data_byte_index = 0;
                            ctr_val = NEXTOR_STATUS_BUSY;
                            read_address = true;
                        } else {
                            block_address = *(uint32_t *)data_response_buffer;
                            ctr_val = NEXTOR_STATUS_BUSY;
                            DRESULT dr = disk_read(pdrv, (BYTE *)data_response_buffer, block_address, 1);
                            if (dr == RES_OK) {
                                data_to_send = 512;
                                data_byte_index = 0;
                                ctr_val = NEXTOR_STATUS_SENDING;
                            } else {
                                data_to_send = 0;
                                ctr_val = NEXTOR_STATUS_ERROR;
                            }
                            read_address = false;
                        }
                        break;
                    case 0x07: // Read next block
                        if (ds & STA_NOINIT) {
                            ctr_val = NEXTOR_STATUS_ERROR;
                            break;
                        }
                        ctr_val = NEXTOR_STATUS_BUSY;
                        block_address++;
                        {
                            DRESULT dr = disk_read(pdrv, (BYTE *)data_response_buffer, block_address, 1);
                            if (dr == RES_OK) {
                                data_to_send = 512;
                                data_byte_index = 0;
                                ctr_val = NEXTOR_STATUS_SENDING;
                            } else {
                                data_to_send = 0;
                                ctr_val = NEXTOR_STATUS_ERROR;
                            }
                        }
                        break;
                    case 0x08: // Write block (address first, then payload)
                        if (ds & STA_NOINIT) {
                            ctr_val = NEXTOR_STATUS_ERROR;
                            break;
                        }
                        if (!read_address) {
                            data_to_receive = 4;
                            data_byte_index = 0;
                            ctr_val = NEXTOR_STATUS_BUSY;
                            read_address = true;
                        } else {
                            block_address = *(uint32_t *)data_response_buffer;
                            read_address = false;
                            data_to_receive = 512;
                            data_byte_index = 0;
                            ctr_val = NEXTOR_STATUS_BUSY;
                        }
                        break;
                    case 0x09: // Write next block
                        if (ds & STA_NOINIT) {
                            ctr_val = NEXTOR_STATUS_ERROR;
                            break;
                        }
                        block_address++;
                        read_address = false;
                        data_to_receive = 512;
                        data_byte_index = 0;
                        ctr_val = NEXTOR_STATUS_BUSY;
                        break;
                    default:
                        break;
                }
            } else if (port == 0x9F) { // Port 0x9F (Data Write): Write data to the buffer.
                if (data_to_receive > 0) {
                    data_response_buffer[data_byte_index++] = busdata; // Store the data in the buffer
                    data_to_receive--; // Decrement the data to receive
                }

                if (!read_address && (data_to_receive == 0) && (data_byte_index >= 512)) { // Full block received for write
                    // Write the block to the SD card
                    ctr_val = NEXTOR_STATUS_BUSY;
                    DRESULT dr = disk_write(pdrv, (BYTE *)data_response_buffer, block_address, 1);
                    if (dr == RES_OK) {
                        ctr_val = NEXTOR_STATUS_READY;
                    } else {
                        ctr_val = NEXTOR_STATUS_ERROR;
                    }
                    data_byte_index = 0;
                }
            }

            wait_for_io_cycle_end(pin_mask_wr, pin_mask_iorq); // Wait until the write is released
            continue;
        }

        if (rd) { // Read transaction: the MSX is reading from the port.
            if (port == 0x9E) { // Port 0x9E (Control Read): Return the control/status register.
                drive_data_bus(ctr_val);
                wait_for_io_cycle_end(pin_mask_rd, pin_mask_iorq);
                release_data_bus();
            } else if (port == 0x9F) {
                if (data_to_send > 0) {
                    out_val = data_response_buffer[data_byte_index++];
                    data_to_send--;
                    drive_data_bus(out_val);
                    ctr_val = NEXTOR_STATUS_SENDING;
                    wait_for_io_cycle_end(pin_mask_rd, pin_mask_iorq);
                    release_data_bus();
                    if (data_to_send == 0) {
                        ctr_val = NEXTOR_STATUS_READY;
                    }
                } else {
                    wait_for_io_cycle_end(pin_mask_rd, pin_mask_iorq);
                }
            } else {
                wait_for_io_cycle_end(pin_mask_rd, pin_mask_iorq);
            }
        }
    }
}
