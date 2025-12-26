// MSX PICOVERSE PROJECT
// (c) 2024 Cristiano Goncalves
// The Retro Hacker
//
// mmapper.c - This is a memory mapper implementation for the MSX
//
// This program implements a simple memory mapper for the MSX computer using the Raspberry Pi Pico.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

// -----------------------
// User-defined pin assignments
// -----------------------
// Address lines (A0-A15) as inputs from MSX
#define PIN_A0     0 
#define PIN_A1     1
#define PIN_A2     2
#define PIN_A3     3
#define PIN_A4     4
#define PIN_A5     5
#define PIN_A6     6
#define PIN_A7     7
#define PIN_A8     8
#define PIN_A9     9
#define PIN_A10    10
#define PIN_A11    11
#define PIN_A12    12
#define PIN_A13    13
#define PIN_A14    14
#define PIN_A15    15

// Data lines (D0-D7)
#define PIN_D0     16
#define PIN_D1     17
#define PIN_D2     18
#define PIN_D3     19
#define PIN_D4     20
#define PIN_D5     21
#define PIN_D6     22
#define PIN_D7     23

// Control signals
#define PIN_RD     24   // Read strobe from MSX
#define PIN_WR     25   // Write strobe from MSX
#define PIN_IORQ   26   // IO Request line from MSX
#define PIN_SLTSL  27   // Slot Select for this cartridge slot
#define PIN_WAIT    28  // WAIT line to MSX 
#define PIN_BUSSDIR 29  // Bus direction line to MSX


// Initialize GPIO pins
static inline void setup_gpio()
{
    // address pins
    gpio_init(PIN_A0);  gpio_set_dir(PIN_A0, GPIO_IN);
    gpio_init(PIN_A1);  gpio_set_dir(PIN_A1, GPIO_IN);
    gpio_init(PIN_A2);  gpio_set_dir(PIN_A2, GPIO_IN);
    gpio_init(PIN_A3);  gpio_set_dir(PIN_A3, GPIO_IN);
    gpio_init(PIN_A4);  gpio_set_dir(PIN_A4, GPIO_IN);
    gpio_init(PIN_A5);  gpio_set_dir(PIN_A5, GPIO_IN);
    gpio_init(PIN_A6);  gpio_set_dir(PIN_A6, GPIO_IN);
    gpio_init(PIN_A7);  gpio_set_dir(PIN_A7, GPIO_IN);
    gpio_init(PIN_A8);  gpio_set_dir(PIN_A8, GPIO_IN);
    gpio_init(PIN_A9);  gpio_set_dir(PIN_A9, GPIO_IN);
    gpio_init(PIN_A10); gpio_set_dir(PIN_A10, GPIO_IN);
    gpio_init(PIN_A11); gpio_set_dir(PIN_A11, GPIO_IN);
    gpio_init(PIN_A12); gpio_set_dir(PIN_A12, GPIO_IN);
    gpio_init(PIN_A13); gpio_set_dir(PIN_A13, GPIO_IN);
    gpio_init(PIN_A14); gpio_set_dir(PIN_A14, GPIO_IN);
    gpio_init(PIN_A15); gpio_set_dir(PIN_A15, GPIO_IN);

    // data pins
    gpio_init(PIN_D0); 
    gpio_init(PIN_D1); 
    gpio_init(PIN_D2); 
    gpio_init(PIN_D3); 
    gpio_init(PIN_D4); 
    gpio_init(PIN_D5); 
    gpio_init(PIN_D6);  
    gpio_init(PIN_D7); 

    // Initialize control pins as input
    gpio_init(PIN_RD); gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR); gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ); gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL); gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);
}


void __no_inline_not_in_flash_func(msxmmapper)(void)
{
    static uint8_t sram_data[128 * 1024];        // 128KB of RAM in the Pico to be “mapped” in 16KB banks
    memset(sram_data, 0x00, sizeof(sram_data));  // Clear the SRAM

    static uint8_t pageRegister[4] = {0, 1, 2, 3};     // Four page registers for 4 pages of 16KB each

    while (true)
    {
        bool sltsl = !(gpio_get(PIN_SLTSL));  // Cartridge slot select
        bool iorq  = !(gpio_get(PIN_IORQ));   // I/O request
        bool rd    = !(gpio_get(PIN_RD));     // Read strobe
        bool wr    = !(gpio_get(PIN_WR));     // Write strobe
        
        if (!sltsl) {
            // Full 16-bit address (A0..A15)
            uint32_t addr_bus = gpio_get_all() & 0xFFFF;  
            // For an I/O operation, typically the low 8 bits are the port number.
            uint8_t  port = (uint8_t)(addr_bus & 0xFF);
            
            if (iorq)
            {
                if (wr && (port >= 0xFC && port <= 0xFF))
                {
                    gpio_set_dir_in_masked(0xFF << 16);   // Set data bus to input mode
                    uint8_t data = (gpio_get_all() >> 16) & 0xFF;  // Get the written value on D0..D7
                    uint8_t page = port - 0xFC;           // Which page register? port 0xFC => page 0, 0xFD => page 1, etc.
                    pageRegister[page] = data & 0x07;     // Only 8 banks in 128KB
                    while (!(gpio_get(PIN_WR))) {         // Wait for WR to go high again (to complete the bus cycle)

                        tight_loop_contents();
                    }
                } else if (rd && (port >= 0xFC && port <= 0xFF))
                {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint8_t page = port - 0xFC;
                    uint8_t data = pageRegister[page];
                    gpio_put_masked(0xFF0000, data << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD))) {                 // Wait for RD to go high again

                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                }
            }
            else
            {
                uint8_t page = (addr_bus >> 14) & 0x03;  // top 2 bits
                uint8_t bank = pageRegister[page];       // which 16KB block for this page

                // offset within that bank
                uint16_t offset_in_bank = addr_bus & 0x3FFF;
                // global offset = bank * 16KB + offset_in_bank
                uint32_t sram_offset = (bank * 0x4000) + offset_in_bank;
                if (rd)
                {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint8_t data = sram_data[sram_offset];  // Read from our local 128KB array
                    gpio_put_masked(0xFF0000, data << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD))) {     // Wait for RD to go high

                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                }
                else if (wr)
                {
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                    uint8_t data = (gpio_get_all() >> 16) & 0xFF;  // Read the byte being written
                    sram_data[sram_offset] = data;   // Store into our local 128KB array
                    while (!(gpio_get(PIN_WR))) {  // Wait for WR to go high
                        tight_loop_contents();
                    }
                }
            }
        }
    }

}

int main()
{
    set_sys_clock_khz(270000, true);
    // Initialize stdio
    stdio_init_all();
    // Initialize GPIO
    setup_gpio();
    msxmmapper();
}
