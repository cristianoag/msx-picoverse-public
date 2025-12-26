// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// loadrom.c - Simple ROM loader for MSX PICOVERSE project - v1.0
//
// This is  small test program that demonstrates how to load simple ROM images using the MSX PICOVERSE project. 
// You need to concatenate the ROM image to the  end of this program binary in order  to load it.
// The program will then act as a simple ROM cartridge that responds to memory read requests from the MSX.
// 
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "loadrom.h"

#include "msx_capture_addr.pio.h"
#include "msx_output_data.pio.h"

PIO pio = pio0;
uint sm0 = 0;  // State machine 0: address capture (using GPIO 0–15)
uint sm1 = 1;  // State machine 1: data output (driving GPIO 16–23)
int dma_chan;
uint32_t dummy_dst[1] = {0};


// Read the address bus from the MSX
static inline uint16_t __not_in_flash_func(read_address_bus)(void) {
    // Return first 16 bits in the most efficient way
    return gpio_get_all() & 0x00FFFF;
}

// Read a byte from the data bus
static inline uint8_t __not_in_flash_func(read_data_bus)(void) 
{
    // Read the data bus
    return (gpio_get_all() >> 16) & 0xFF;
}

// Write a byte to the data bus
static inline void __not_in_flash_func(write_data_bus)(uint8_t data) {
    // Write the given byte to the given address
    gpio_put_masked(0xFF0000, data << 16);
}

// Set the data bus to input mode
static inline void __not_in_flash_func(set_data_bus_input)(void) {
    // Set data lines to input (high impedance)
    gpio_set_dir(PIN_D0, GPIO_IN);
    gpio_set_dir(PIN_D1, GPIO_IN);
    gpio_set_dir(PIN_D2, GPIO_IN);
    gpio_set_dir(PIN_D3, GPIO_IN);
    gpio_set_dir(PIN_D4, GPIO_IN);
    gpio_set_dir(PIN_D5, GPIO_IN);
    gpio_set_dir(PIN_D6, GPIO_IN);
    gpio_set_dir(PIN_D7, GPIO_IN);

    //gpio_set_dir_in_masked(0xFF << 16);
}

// Set the data bus to output mode
static inline void __not_in_flash_func(set_data_bus_output)(void)
{
    gpio_set_dir(PIN_D0, GPIO_OUT);
    gpio_set_dir(PIN_D1, GPIO_OUT);
    gpio_set_dir(PIN_D2, GPIO_OUT);
    gpio_set_dir(PIN_D3, GPIO_OUT);
    gpio_set_dir(PIN_D4, GPIO_OUT);
    gpio_set_dir(PIN_D5, GPIO_OUT);
    gpio_set_dir(PIN_D6, GPIO_OUT);
    gpio_set_dir(PIN_D7, GPIO_OUT);

    //gpio_set_dir_out_masked(0xFF << 16);

}

static inline void setup_data_gpio()
{
    // Data pins
    gpio_init(PIN_D0); 
    gpio_init(PIN_D1); 
    gpio_init(PIN_D2); 
    gpio_init(PIN_D3); 
    gpio_init(PIN_D4); 
    gpio_init(PIN_D5); 
    gpio_init(PIN_D6);  
    gpio_init(PIN_D7); 
}
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
    gpio_init(PIN_IORQ); gpio_set_dir(PIN_IORQ, GPIO_IN); gpio_pull_up(PIN_IORQ);
    gpio_init(PIN_SLTSL); gpio_set_dir(PIN_SLTSL, GPIO_IN); gpio_pull_up(PIN_SLTSL);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN); gpio_pull_up(PIN_BUSSDIR);
}

void __no_inline_not_in_flash_func(sm0_irq_handler_plain32)() {

    if (!pio_sm_is_rx_fifo_empty(pio, sm0)) {
        uint32_t addr_val = pio_sm_get_blocking(pio, sm0);
        uint16_t addr = (uint16_t) (addr_val>>16);        

        if (addr >= 0x4000 && addr <= 0xBFFF) {

            uint32_t rom_addr = 0x1d + (addr - 0x4000);
            dma_channel_set_read_addr(dma_chan, rom+rom_addr, true);
            
            //debug
            /*uint8_t data_received_from_sm1 = pio_sm_get_blocking(pio, sm1);
            printf("Data received from SM1: %x\n", data_received_from_sm1);
            dma_channel_wait_for_finish_blocking(dma_chan);
            printf("Data transferred via DMA: %x\n", dummy_dst[0]);*/
        }
        
    }
}

void setup_pio_capture_addr() {
    
    // Initialize the control signals and address pins as PIO GPIOs:
    pio_gpio_init(pio, PIN_RD);  // /RD
    pio_gpio_init(pio, PIN_SLTSL);  // /SLTSL
    for (int i = 0; i < 16; i++) pio_gpio_init(pio, ADDR_PINS + i);

    // Load the PIO program
    uint offset0 = pio_add_program(pio, &msx_capture_addr_program);
    pio_sm_config c0 = msx_capture_addr_program_get_default_config(offset0);

    sm_config_set_in_pins(&c0, PIN_A0);  // Configure input pins
    sm_config_set_wrap(&c0, offset0, offset0 + msx_capture_addr_program.length - 1); 
    sm_config_set_fifo_join(&c0, PIO_FIFO_JOIN_NONE);  // Separate RX and TX FIFOs
    sm_config_set_clkdiv(&c0, 1.0f);  // MSX bus timing adjust

    irq_set_exclusive_handler(PIO0_IRQ_0, sm0_irq_handler_plain32);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_set_irq0_source_enabled(pio, pis_sm0_rx_fifo_not_empty, true);

    pio_sm_init(pio, sm0, offset0, &c0);
    pio_sm_set_enabled(pio, sm0, true);

}

void setup_pio_output_data() {

    for (int i = 0; i < 8; i++) pio_gpio_init(pio, DATA_PINS + i);

    // ----- Set up SM1 for data output -----
    uint offset1 = pio_add_program(pio, &msx_output_data_program);
    pio_sm_config c1 = msx_output_data_program_get_default_config(offset1);
    // Configure the 'out' pins: data bus on GPIO 16–23.
    sm_config_set_out_pins(&c1, DATA_PINS, 8);
    // Enable autopull so SM1 automatically pulls an 8-bit value when needed.
    sm_config_set_out_shift(&c1, true, true, 8);
    pio_sm_init(pio, sm1, offset1, &c1);
    //pio_sm_clear_fifos(pio, sm1);        //Clear FIFO buffers
    pio_sm_set_enabled(pio, sm1, true);

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8); // Byte transfer
    channel_config_set_read_increment(&c, false); // No increment for ROM data
    channel_config_set_write_increment(&c, false); // No increment for PIO TX FIFO
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm1, true)); // Pace transfer with PIO TX FIFO

    dma_channel_configure(
        dma_chan,
        &c,
        &pio->txf[sm1], // Write data to PIO SM1 TX FIFO
        NULL, // Read address to be set in IRQ handler
        1, // Transfer one byte
        false // Don't start yet
    );



}



// Dump the ROM data in hexdump format
// debug function
void dump_rom_sram(uint32_t size)
{
    // Dump the ROM data in hexdump format
    for (int i = 0; i < size; i += 16) {
        // Print the address offset
        printf("%08x  ", i);

        // Print 16 bytes of data in hexadecimal
        for (int j = 0; j < 16 && (i + j) < size; j++) {
            printf("%02x ", rom_sram[i + j]);
        }

        // Add spacing if the last line has fewer than 16 bytes
        for (int j = size - i; j < 16 && i + j < size; j++) {
            printf("   ");
        }

        // Print the ASCII representation of the data
        printf(" |");
        for (int j = 0; j < 16 && (i + j) < size; j++) {
            char c = rom_sram[i + j];
            if (c >= 32 && c <= 126) {
                printf("%c", c);  // Printable ASCII character
            } else {
                printf(".");      // Non-printable character
            }
        }
        printf("|\n");
    }
}

void dump_rom(uint32_t size)
{
    // Dump the ROM data in hexdump format
    for (int i = 0; i < size; i += 16) {
        // Print the address offset
        printf("%08x  ", i);

        // Print 16 bytes of data in hexadecimal
        for (int j = 0; j < 16 && (i + j) < size; j++) {
            printf("%02x ", rom[i + j]);
        }

        // Add spacing if the last line has fewer than 16 bytes
        for (int j = size - i; j < 16 && i + j < size; j++) {
            printf("   ");
        }

        // Print the ASCII representation of the data
        printf(" |");
        for (int j = 0; j < 16 && (i + j) < size; j++) {
            char c = rom[i + j];
            if (c >= 32 && c <= 126) {
                printf("%c", c);  // Printable ASCII character
            } else {
                printf(".");      // Non-printable character
            }
        }
        printf("|\n");
    }
}

// loadrom_plain32 - Load a simple 32KB (or less) ROM into the MSX directly from the pico flash
// 32KB ROMS have two pages of 16Kb each in the following areas:
// 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x0000, 0x0001
// 16KB ROMS have only one page in the 0x4000-0x7FFF area
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_plain32)(uint32_t offset)
{
    // Pre-calculate the constant mask for the address bus.
    const uint32_t ADDR_MASK = 0x00FFFF;
    const uint32_t DATA_MASK = 0xFF << 16;

    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
    while (true) 
    {
        // Read entire GPIO state once (assuming gpio_get_all is fast enough)
        uint32_t gpio_state = gpio_get_all();

        // Extract individual signals; adjust these bit positions if needed
        bool sltsl = !(gpio_state & (1 << PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_state & (1 << PIN_RD));         // Read cycle (active low)

        if (sltsl && rd) 
        {
            uint16_t addr = gpio_state & ADDR_MASK;
            if (addr >= 0x4000 && addr <= 0xBFFF)
            {
                uint32_t rom_addr = offset + (addr - 0x4000);
                
                // Only switch if necessary. Set bus to output mode
                gpio_set_dir_out_masked(DATA_MASK);
                
                // Write the data directly from ROM (with the appropriate shift)
                uint32_t data = rom[rom_addr] << 16;
                gpio_put_masked(DATA_MASK, data);
                
                // Wait for RD to go high (read cycle complete)
                while (!(gpio_get(PIN_RD))) 
                {
                    tight_loop_contents();
                }
                
                // Set data bus back to input mode after cycle completes
                gpio_set_dir_in_masked(DATA_MASK);
            }
        }
    }
}

// Tests with PIO and DMA when loading 32KB roms
void __no_inline_not_in_flash_func(loadrom_plain32_pio)(uint32_t offset)
{
    setup_pio_capture_addr();
    setup_pio_output_data();

    while (true)
    {
        tight_loop_contents();
    }
    
}

void __no_inline_not_in_flash_func(loadrom_plain32_dma)(uint32_t offset)
{

    uint32_t dummy_dst[1];

    memset(rom_sram, 0, 32768); // Clear the SRAM buffer
    memcpy(rom_sram, rom + offset, 32768); //for 32KB ROMs we start at 0x4000

    int dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_FORCE);

    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
    while (true) 
    {
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)

        if (sltsl) 
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus

            if (addr >= 0x4000 && addr <= 0xBFFF) // Check if the address is within the ROM range
            {
                //printf("addr: %x\n", addr);
                if (rd) // Handle read requests within the ROM address range
                {
                    uint32_t rom_addr = (addr - 0x4000); // Calculate flash address
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode

                    uint32_t data = rom_sram[rom_addr] << 16;
                    //printf("data: %x\n", data);

                    dma_channel_configure(
                        dma_chan, 
                        &c,
                        (void *)(SIO_BASE + SIO_GPIO_OUT_OFFSET), // Destination: GPIO output
                        //dummy_dst, // Destination: GPIO output
                        &data, // Source: Flash ROM
                        1, // Transfer a single byte
                        true // Start transfer immediately
                    );

                    dma_channel_wait_for_finish_blocking(dma_chan);

                    //printf("data after: %x\n", dummy_dst[0]);

                    //gpio_put_masked(0xFF0000, dummy_dst[0]); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD)))  // Wait until the read cycle completes (RD goes high)
                    {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode after cycle completes
                }
            } 
        } 
    }
}


// loadrom_plain32 - Load a simple 32KB (or less) ROM into the MSX using SRAM buffer
// 32KB ROMS have two pages of 16Kb each in the following areas:
// 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x0000, 0x0001
// 16KB ROMS have only one page in the 0x4000-0x7FFF area
// AB is on 0x0000, 0x0001
void loadrom_plain32_sram(PIO pio, uint sm_addr, uint32_t offset, uint32_t size)
{
    //setup the rom_sram buffer for the 32KB ROM
    gpio_init(PIN_WAIT); // Init wait signal pin
    gpio_set_dir(PIN_WAIT, GPIO_OUT); // Set the WAIT signal as output
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, size); // Clear the SRAM buffer
    memcpy(rom_sram + 0x4000, rom + offset, size); //for 32KB ROMs we start at 0x4000
    gpio_put(PIN_WAIT, 1); // Lets go!

    // Set data bus to input mode
    set_data_bus_input();
    while (true) 
    {
         // Check control signals
         bool sltsl = (gpio_get(PIN_SLTSL) == 0); // Slot select (active low)
         bool rd = (gpio_get(PIN_RD) == 0);       // Read cycle (active low)

         if (sltsl) {

            uint16_t addr = read_address_bus();
            if (addr >= 0x4000 && addr <= 0xBFFF) // Check if the address is within the ROM range
            {
                if (rd)
                {
                    set_data_bus_output(); // Drive data bus to output mode
                    write_data_bus(rom_sram[addr]);  // Drive data onto the bus
                    while (gpio_get(PIN_RD) == 0)  // Wait until the read cycle completes (RD goes high)
                    {
                        tight_loop_contents();
                    }
                    set_data_bus_input(); // Return data lines to input mode after cycle completes
                }
            } 
         }
    }
}


// loadrom_linear48 - Load a simple 48KB Linear0 ROM into the MSX directly from the pico flash
// Those ROMs have three pages of 16Kb each in the following areas:
// 0x0000-0x3FFF, 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x4000, 0x4001
void __no_inline_not_in_flash_func(loadrom_linear48)(uint32_t offset)
{
    const uint32_t DATA_BUS_MASK = 0xFF << 16;
    const uint32_t ADDR_MASK = 0x00FFFF;
    
    // Initially set the data bus to input mode.
    gpio_set_dir_in_masked(DATA_BUS_MASK);

    while (true) 
    {
        // Read the entire GPIO state once per loop iteration.
        uint32_t gpio_state = gpio_get_all();

        // Extract control signals from the read state.
        bool sltsl = !(gpio_state & (1 << PIN_SLTSL)); // Slot selected (active low)
        bool rd    = !(gpio_state & (1 << PIN_RD));    // Read cycle (active low)

        // Proceed only if both slot select and read are active.
        if (sltsl && rd)
        {
            // Get the address bus value.
            uint16_t addr = gpio_state & ADDR_MASK;
            // Since addr is always >= 0, we only check the upper bound.
            if (addr <= 0xBFFF)
            {
                uint32_t rom_addr = offset + addr;
                // Switch the data bus to output mode.
                gpio_set_dir_out_masked(DATA_BUS_MASK);
                // Write the ROM data (shifted to match the data bus position).
                gpio_put_masked(DATA_BUS_MASK, rom[rom_addr] << 16);

                // Wait until the read cycle completes (RD goes high).
                while ((gpio_get_all() & (1 << PIN_RD)) == 0)
                {
                    tight_loop_contents();
                }
                // Return the data bus to input mode.
                gpio_set_dir_in_masked(DATA_BUS_MASK);
            }
        }
    }
}

// loadrom_linear48 - Load a simple 48KB Linear0 ROM into the MSX using SRAM buffer
// Those ROMs have three pages of 16Kb each in the following areas:
// 0x0000-0x3FFF, 0x4000-0x7FFF and 0x8000-0xBFFF
// AB is on 0x4000, 0x4001
void loadrom_linear48_sram(uint32_t offset, uint32_t size)
{
    // load the ROM into the SRAM buffer
    gpio_init(PIN_WAIT); gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, size); // Clear the SRAM buffer
    memcpy(rom_sram, rom + offset, size);  // for 48KB Linear0 ROMs we start at 0x0000
    gpio_put(PIN_WAIT, 1); // Lets go!

    set_data_bus_input();     // Set data bus to input mode
    while (true) 
    {
        // Check control signals
        bool sltsl = (gpio_get(PIN_SLTSL) == 0); // Slot select (active low)
        bool rd = (gpio_get(PIN_RD) == 0);       // Read cycle (active low)

        if (sltsl)
        {
            uint16_t addr = read_address_bus();
            if (rd)
            {
                if (addr >= 0x0000 && addr <= 0xBFFF) // Check if the address is within the ROM range
                {
                    set_data_bus_output();  // Drive data bus to output mode
                    write_data_bus(rom_sram[addr]); // Drive data onto the bus
                    while (gpio_get(PIN_RD) == 0) // Wait until the read cycle completes (RD goes high)
                    {
                        tight_loop_contents();
                    }
                    set_data_bus_input(); // Return data lines to input mode after cycle completes
                }
            }
        }
    }
}


// loadrom_konamiscc - Load a any Konami SCC ROM into the MSX directly from the pico flash
// The KonamiSCC ROMs are divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments 
// into the MSX's address space. Since the size of the mapper is 8Kb, the memory banks are:
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
// And the address to change banks are:
// Bank 1: 5000h - 57FFh (5000h used), Bank 2: 7000h - 77FFh (7000h used), Bank 3: 9000h - 97FFh (9000h used), Bank 4: B000h - B7FFh (B000h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_konamiscc)(uint32_t offset)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped

    const uint32_t DATA_MASK = 0xFF << 16;
    const uint32_t ADDR_MASK = 0x00FFFF;

    gpio_set_dir_in_masked(DATA_MASK); // Set data bus to input mode
    while (true) 
    {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        
        if (sltsl)
        {
            uint32_t gpio_state = gpio_get_all();

            uint16_t addr = gpio_state & ADDR_MASK; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF)  // Check if the address is within the ROM range
            {
                bool rd = !(gpio_state & (1 << PIN_RD));
                bool wr = !(gpio_state & (1 << PIN_WR));

                if (rd) 
                {
                    gpio_set_dir_out_masked(DATA_MASK); // Set data bus to output mode
                    uint32_t rom_offset = offset + (bank_registers[(addr - 0x4000) >> 13] * 0x2000) + (addr & 0x1FFF); // Calculate the ROM offset
                    uint32_t data = rom[rom_offset] << 16; // Read the data from the ROM
                    gpio_put_masked(0xFF0000, data); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD)))  {tight_loop_contents();}
             
                    gpio_set_dir_in_masked(DATA_MASK); // Return data bus to input mode after cycle completes
                }  else if (wr) 
                {
                    // Handle writes to bank switching addresses
                    if ((addr >= 0x5000)  && (addr <= 0x57FF)) { 
                        bank_registers[0] = (gpio_state >> 16) & 0xFF;
                    } else if ((addr >= 0x7000) && (addr <= 0x77FF)) {
                        bank_registers[1] = (gpio_state >> 16) & 0xFF;
                    } else if ((addr >= 0x9000) && (addr <= 0x97FF)) {
                        bank_registers[2] = (gpio_state >> 16) & 0xFF;
                    } else if ((addr >= 0xB000) && (addr <= 0xB7FF)) {
                        bank_registers[3] = (gpio_state >> 16) & 0xFF;
                    }

                    while (!(gpio_get(PIN_WR))) {tight_loop_contents();}
                   
                }  
            }
        }
    }
}



// loadrom_konamiscc - Load a Konami SCC ROM (maximum of 128KB) into the MSX using SRAM buffer
// The KonamiSCC ROMs are divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments 
// into the MSX's address space. Since the size of the mapper is 8Kb, the memory banks are:
//
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
//
// And the address to change banks are:
//
// Bank 1: 5000h - 57FFh (5000h used), Bank 2: 7000h - 77FFh (7000h used), Bank 3: 9000h - 97FFh (9000h used), Bank 4: B000h - B7FFh (B000h used)
// AB is on 0x0000, 0x0001
void loadrom_konamiscc_sram(uint32_t offset, uint32_t size)
{
    if (size > MAX_MEM_SIZE)
    {
        return;
    }

    // Load the ROM into the SRAM buffer
    gpio_init(PIN_WAIT); gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, size+offset); // Clear the SRAM buffer
    memcpy(rom_sram, rom, size+offset);  // for 128KB KonamiSCC ROMs we start at 0x0000
    gpio_put(PIN_WAIT, 1); // Lets go!

    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped

    set_data_bus_input();
    while (true) 
    {
        bool sltsl = (gpio_get(PIN_SLTSL) == 0); // Slot selected (active low)
        bool rd = (gpio_get(PIN_RD) == 0);       // Read cycle (active low)
        bool wr = (gpio_get(PIN_WR) == 0);       // Write cycle (active low)

        if (sltsl)
        {
            uint16_t addr = read_address_bus();
            if (addr >= 0x4000 && addr <= 0xBFFF) 
            {
                if (rd) 
                {
                    uint16_t bank_index = (addr - 0x4000) / 0x2000; // Calculate the bank index
                    uint16_t bank_offset = addr & 0x1FFF; // Calculate the offset within the bank
                    uint32_t rom_offset = (bank_registers[bank_index] * 0x2000) + bank_offset + offset; // Calculate the ROM offset
                    set_data_bus_output();
                    write_data_bus(rom_sram[rom_offset]); // Drive data onto the bus
                    while (gpio_get(PIN_RD) == 0) 
                    {
                        tight_loop_contents();
                    }
                    set_data_bus_input();
                } else if (wr) 
                {
                    // Handle writes to bank switching addresses
                    if (addr == 0x5000) {
                        bank_registers[0] = read_data_bus(); // Read the data bus and store in bank register
                    } else if (addr == 0x7000) {
                        bank_registers[1] = read_data_bus();
                    } else if (addr == 0x9000) {
                        bank_registers[2] = read_data_bus();
                    } else if (addr == 0xB000) {
                        bank_registers[3] = read_data_bus();
                    }

                    while (!(gpio_get(PIN_WR)))
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// loadrom_konami - Load a Konami (without SCC) ROM into the MSX directly from the pico flash
// The Konami (without SCC) ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
//
//  Bank 1: 4000h - 5FFFh, Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
//
// And the addresses to change banks are:
//
//	Bank 1: <none>, Bank 2: 6000h - 67FFh (6000h used), Bank 3: 8000h - 87FFh (8000h used), Bank 4: A000h - A7FFh (A000h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_konami)(uint32_t offset)
{
    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped

    gpio_set_dir_in_masked(0xFF << 16);
    while (true) 
    {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl)
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF) 
            {
                if (rd) 
                {
                    gpio_set_dir_out_masked(0xFF << 16);
                    uint32_t rom_offset = offset + (bank_registers[(addr - 0x4000) >> 13] * 0x2000) + (addr & 0x1FFF); // Calculate the ROM offset
                    gpio_put_masked(0xFF0000, rom[rom_offset] << 16);
                    while (!(gpio_get(PIN_RD))) 
                    {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16);

                }else if (wr) {
                    // Handle writes to bank switching addresses
                    if ((addr >= 0x6000) && (addr <= 0x67FF)) {
                        bank_registers[1] = read_data_bus();
                    } else if ((addr >= 0x8000) && (addr <= 0x87FF)) {
                        bank_registers[2] = read_data_bus();
                    } else if ((addr >= 0xA000) && (addr <= 0xA7FF)) {
                        bank_registers[3] = read_data_bus();
                    }

                    while (!(gpio_get(PIN_WR))) 
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}


// loadrom_konami - Load a Konami (without SCC) ROM (MAXIMUM of 128KB) into the MSX using SRAM buffer
// The Konami (without SCC) ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
//
// Bank 1: 4000h - 5FFFh, Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
//
// And the addresses to change banks are:
//
// Bank 1: <none>, Bank 2: 6000h - 67FFh (6000h used), Bank 3: 8000h - 87FFh (8000h used), Bank 4: A000h - A7FFh (A000h used)
// AB is on 0x0000, 0x0001
void loadrom_konami_sram(uint32_t offset, uint32_t size)
{
    gpio_init(PIN_WAIT); gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, size+offset); // Clear the SRAM buffer
    memcpy(rom_sram, rom, size+offset);  // for 12KB KonamiSCC ROMs we start at 0x0000
    gpio_put(PIN_WAIT, 1); // Lets go!

    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped

    set_data_bus_input();
    while (true) 
    {
        bool sltsl = (gpio_get(PIN_SLTSL) == 0); // Slot selected (active low)
        bool rd = (gpio_get(PIN_RD) == 0);       // Read cycle (active low)
        bool wr = (gpio_get(PIN_WR) == 0);       // Write cycle (active low)

        if (sltsl) {
            uint16_t addr = read_address_bus();

            if (rd) {
                // Handle read requests within the ROM address range
                if (addr >= 0x4000 && addr <= 0xBFFF) {
                    // Determine the bank index based on the address
                    uint8_t bank_index = (addr - 0x4000) / 0x2000;
                    uint16_t bank_offset = addr & 0x1FFF;
                    uint32_t rom_offset = (bank_registers[bank_index] * 0x2000) + bank_offset + offset;

                    // Set data bus to output mode and write the data
                    set_data_bus_output();
                    write_data_bus(rom_sram[rom_offset]);

                    // Wait for the read cycle to complete
                    while (gpio_get(PIN_RD) == 0) {
                        tight_loop_contents();
                    }

                    // Return data bus to input mode after the read cycle
                    set_data_bus_input();
                }
            } else if (wr) {
                // Handle writes to bank switching addresses
                if (addr == 0x6000) {
                    bank_registers[1] = read_data_bus();
                } else if (addr == 0x8000) {
                    bank_registers[2] = read_data_bus();
                } else if (addr == 0xA000) {
                    bank_registers[3] = read_data_bus();
                }

                while (!(gpio_get(PIN_WR))) {
                    tight_loop_contents();
                }
            }
        }
    }
}

// loadrom_ascii8 - Load an ASCII8 ROM into the MSX directly from the pico flash
// The ASCII8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
// 
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
//
// And the address to change banks are:
// 
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 6800h - 6FFFh (6800h used), Bank 3: 7000h - 77FFh (7000h used), Bank 4: 7800h - 7FFFh (7800h used)
// AB is on 0x0000, 0x0001
void __no_inline_not_in_flash_func(loadrom_ascii8)(uint32_t offset)
{

    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped

    gpio_set_dir_in_masked(0xFF << 16);
    while (true) 
    {
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl) {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF) 
            {
                if (rd) 
                {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint32_t rom_offset = offset + (bank_registers[(addr - 0x4000) >> 13] * 0x2000) + (addr & 0x1FFF); // Calculate the ROM offset
                    gpio_put_masked(0xFF0000, rom[rom_offset] << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD)))  { // Wait for the read cycle to complete
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode after the read cycle
                } else if (wr)  // Handle writes to bank switching addresses
                { 
                    if ((addr >= 0x6000) && (addr <= 0x67FF)) { 
                        bank_registers[0] = read_data_bus(); // Read the data bus and store in bank register
                        //printf("Bank 1: %d\n", bank_registers[0]);
                    } else if ((addr >= 0x6800) && (addr <= 0x6FFF)) {
                        bank_registers[1] = read_data_bus();
                        //printf("Bank 2: %d\n", bank_registers[1]);
                    } else if ((addr >= 0x7000) && (addr <= 0x77FF)) {
                        bank_registers[2] = read_data_bus();
                        //printf("Bank 3: %d\n", bank_registers[2]);
                    } else if ((addr >= 0x7800) && (addr <= 0x7FFF)) {
                        bank_registers[3] = read_data_bus();
                        //printf("Bank 4: %d\n", bank_registers[3]);
                    }

                    while (!(gpio_get(PIN_WR))) 
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// loadrom_ascii8 - Load an ASCII8 ROM into the MSX using SRAM buffer
// The ASCII8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 8Kb, the memory banks are:
// 
// Bank 1: 4000h - 5FFFh , Bank 2: 6000h - 7FFFh, Bank 3: 8000h - 9FFFh, Bank 4: A000h - BFFFh
//
// And the address to change banks are:
// 
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 6800h - 6FFFh (6800h used), Bank 3: 7000h - 77FFh (7000h used), Bank 4: 7800h - 7FFFh (7800h used)
// AB is on 0x0000, 0x0001
void loadrom_ascii8_sram(uint32_t offset, uint32_t size)
{
    gpio_init(PIN_WAIT); gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0); // Wait until we are ready to read the ROM
    memset(rom_sram, 0, MAX_MEM_SIZE); // Clear the SRAM buffer
    memcpy(rom_sram, rom + offset, size);  // for 12KB KonamiSCC ROMs we start at 0x0000
    gpio_put(PIN_WAIT, 1); // Lets go!

    uint8_t bank_registers[4] = {0, 1, 2, 3}; // Initial banks 0-3 mapped

    set_data_bus_input();
    while (true) 
    {
        bool sltsl = (gpio_get(PIN_SLTSL) == 0); // Slot selected (active low)
        bool rd = (gpio_get(PIN_RD) == 0);       // Read cycle (active low)
        bool wr = (gpio_get(PIN_WR) == 0);       // Write cycle (active low)

        if (sltsl) {
            uint16_t addr = read_address_bus();

            if (rd) {
                // Handle read requests within the ROM address range
                if (addr >= 0x4000 && addr <= 0xBFFF) {
                    // Determine the bank index based on the address
                    uint32_t rom_offset = (bank_registers[(addr - 0x4000) / 0x2000] * 0x2000) + (addr & 0x1FFF);

                    // Set data bus to output mode and write the data
                    set_data_bus_output();
                    write_data_bus(rom_sram[rom_offset]);

                    // Wait for the read cycle to complete
                    while (gpio_get(PIN_RD) == 0) {
                        tight_loop_contents();
                    }

                    // Return data bus to input mode after the read cycle
                    set_data_bus_input();
                }
            } else if (wr) {
                // Handle writes to bank switching addresses
                if (addr == 0x6000) {
                    bank_registers[0] = read_data_bus(); // Read the data bus and store in bank register
                } else if (addr == 0x6800) {
                    bank_registers[1] = read_data_bus();
                } else if (addr == 0x7000) {
                    bank_registers[2] = read_data_bus();
                } else if (addr == 0x7800) {
                    bank_registers[3] = read_data_bus();
                }
                while (!(gpio_get(PIN_WR))) {
                    tight_loop_contents();
                }
            }
        }
    }
}

// loadrom_ascii16 - Load an ASCII16 ROM into the MSX directly from the pico flash
// The ASCII16 ROM is divided into 16KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Since the size of the mapper is 16Kb, the memory banks are:
//
// Bank 1: 4000h - 7FFFh , Bank 2: 8000h - BFFFh
//
// And the address to change banks are:
// Bank 1: 6000h - 67FFh (6000h used), Bank 2: 7000h - 77FFh (7000h and 77FFh used)
void __no_inline_not_in_flash_func(loadrom_ascii16)(uint32_t offset)
{
    uint8_t bank_registers[2] = {0, 1}; // Initial banks 0 and 1 mapped

    gpio_set_dir_in_masked(0xFF << 16);
    while (true) {
        // Check control signals
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl) {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read the address bus
            if (addr >= 0x4000 && addr <= 0xBFFF)  
            {
                if (rd) {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    uint32_t rom_offset = offset + (bank_registers[(addr >> 15) & 1] << 14) + (addr & 0x3FFF);
                    gpio_put_masked(0xFF0000, rom[rom_offset] << 16); // Write the data to the data bus
                    while (!(gpio_get(PIN_RD)))  // Wait for the read cycle to complete
                    {
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode after the read cycle
                }
                else if (wr) 
                {
                    // Update bank registers based on the specific switching addresses
                    if ((addr >= 0x6000) && (addr <= 0x67FF)) {
                        bank_registers[0] = (gpio_get_all() >> 16) & 0xFF;
                    } else if (addr >= 0x7000 && addr <= 0x77FF) {
                        bank_registers[1] = (gpio_get_all() >> 16) & 0xFF;
                    }
                    while (!(gpio_get(PIN_WR))) {
                        tight_loop_contents();
                    }
                }
                
            }
        }
    }
}

// loadrom_neo8 - Load an NEO8 ROM into the MSX directly from the pico flash
// The NEO8 ROM is divided into 8KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Size of a segment: 8 KB
// Segment switching addresses:
// Bank 0: 0000h~1FFFh, Bank 1: 2000h~3FFFh, Bank 2: 4000h~5FFFh, Bank 3: 6000h~7FFFh, Bank 4: 8000h~9FFFh, Bank 5: A000h~BFFFh
// Switching address: 
// 5000h (mirror at 1000h, 9000h and D000h), 
// 5800h (mirror at 1800h, 9800h and D800h), 
// 6000h (mirror at 2000h, A000h and E000h), 
// 6800h (mirror at 2800h, A800h and E800h), 
// 7000h (mirror at 3000h, B000h and F000h), 
// 7800h (mirror at 3800h, B800h and F800h)
void __no_inline_not_in_flash_func(loadrom_neo8)(uint32_t offset)
{
    uint16_t bank_registers[6] = {0}; // 16-bit bank registers initialized to zero (12-bit segment, 4 MSB reserved)

    gpio_set_dir_in_masked(0xFF << 16);    // Configure GPIO pins for input mode
    while (true)
    {
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl)
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read address bus

            if (addr <= 0xBFFF)
            {
                if (rd)
                {
                    // Handle read access
                    gpio_set_dir_out_masked(0xFF << 16); // Data bus output mode
                    uint8_t bank_index = addr >> 13;     // Determine bank index (0-5)

                    if (bank_index < 6)
                    {
                        uint32_t segment = bank_registers[bank_index] & 0x0FFF; // 12-bit segment number
                        uint32_t rom_offset = offset + (segment << 13) + (addr & 0x1FFF); // Calculate ROM offset
                        gpio_put_masked(0xFF0000, rom[rom_offset] << 16); // Place data on data bus
                    }
                    else
                    {
                        gpio_put_masked(0xFF0000, 0xFF << 16); // Invalid page handling (Page 3)
                    }

                    while (!(gpio_get(PIN_RD))) // Wait for read cycle to complete
                    {
                        tight_loop_contents();
                    }

                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode
                }
                else if (wr)
                {
                    // Handle write access
                    uint16_t base_addr = addr & 0xF800; // Mask to identify base address
                    uint8_t bank_index = 6;             // Initialize to invalid bank

                    // Determine bank index based on base address
                    switch (base_addr)
                    {
                        case 0x5000:
                        case 0x1000:
                        case 0x9000:
                        case 0xD000:
                            bank_index = 0;
                            break;
                        case 0x5800:
                        case 0x1800:
                        case 0x9800:
                        case 0xD800:
                            bank_index = 1;
                            break;
                        case 0x6000:
                        case 0x2000:
                        case 0xA000:
                        case 0xE000:
                            bank_index = 2;
                            break;
                        case 0x6800:
                        case 0x2800:
                        case 0xA800:
                        case 0xE800:
                            bank_index = 3;
                            break;
                        case 0x7000:
                        case 0x3000:
                        case 0xB000:
                        case 0xF000:
                            bank_index = 4;
                            break;
                        case 0x7800:
                        case 0x3800:
                        case 0xB800:
                        case 0xF800:
                            bank_index = 5;
                            break;
                    }

                    if (bank_index < 6)
                    {
                        uint8_t data = read_data_bus() & 0xFF; // Read 8-bit data from bus
                        if (addr & 0x01)
                        {
                            // Write to MSB
                            bank_registers[bank_index] = (bank_registers[bank_index] & 0x00FF) | (data << 8);
                        }
                        else
                        {
                            // Write to LSB
                            bank_registers[bank_index] = (bank_registers[bank_index] & 0xFF00) | data;
                        }

                        // Ensure reserved MSB bits are zero
                        bank_registers[bank_index] &= 0x0FFF;
                    }

                    while (!(gpio_get(PIN_WR))) // Wait for write cycle to complete
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// loadrom_neo16 - Load an NEO16 ROM into the MSX directly from the pico flash
// The NEO16 ROM is divided into 16KB segments, managed by a memory mapper that allows dynamic switching of these segments into the MSX's address space
// Size of a segment: 16 KB
// Segment switching addresses:
// Bank 0: 0000h~3FFFh, Bank 1: 4000h~7FFFh, Bank 2: 8000h~BFFFh
// Switching address:
// 5000h (mirror at 1000h, 9000h and D000h),
// 6000h (mirror at 2000h, A000h and E000h),
// 7000h (mirror at 3000h, B000h and F000h)
void __no_inline_not_in_flash_func(loadrom_neo16)(uint32_t offset)
{
    // 16-bit bank registers initialized to zero (12-bit segment, 4 MSB reserved)
    uint16_t bank_registers[3] = {0};

    // Configure GPIO pins for input mode
    gpio_set_dir_in_masked(0xFF << 16);
    while (true)
    {
        bool sltsl = !(gpio_get(PIN_SLTSL)); // Slot selected (active low)
        bool rd = !(gpio_get(PIN_RD));       // Read cycle (active low)
        bool wr = !(gpio_get(PIN_WR));       // Write cycle (active low)

        if (sltsl)
        {
            uint16_t addr = gpio_get_all() & 0x00FFFF; // Read address bus
            if (addr <= 0xBFFF)
            {
                if (rd)
                {
                    // Handle read access
                    gpio_set_dir_out_masked(0xFF << 16); // Data bus output mode
                    uint8_t bank_index = addr >> 14;     // Determine bank index (0-2)

                    if (bank_index < 3)
                    {
                        uint32_t segment = bank_registers[bank_index] & 0x0FFF; // 12-bit segment number
                        uint32_t rom_offset = offset + (segment << 14) + (addr & 0x3FFF); // Calculate ROM offset
                        gpio_put_masked(0xFF0000, rom[rom_offset] << 16); // Place data on data bus
                    }
                    else
                    {
                        gpio_put_masked(0xFF0000, 0xFF << 16); // Invalid page handling
                    }

                    while (!(gpio_get(PIN_RD))) // Wait for read cycle to complete
                    {
                        tight_loop_contents();
                    }

                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode
                }
                else if (wr)
                {
                    // Handle write access
                    uint16_t base_addr = addr & 0xF800; // Mask to identify base address
                    uint8_t bank_index = 3;             // Initialize to invalid bank

                    // Determine bank index based on base address
                    switch (base_addr)
                    {
                        case 0x5000:
                        case 0x1000:
                        case 0x9000:
                        case 0xD000:
                            bank_index = 0;
                            break;
                        case 0x6000:
                        case 0x2000:
                        case 0xA000:
                        case 0xE000:
                            bank_index = 1;
                            break;
                        case 0x7000:
                        case 0x3000:
                        case 0xB000:
                        case 0xF000:
                            bank_index = 2;
                            break;
                    }

                    if (bank_index < 3)
                    {
                        uint8_t data = read_data_bus() & 0xFF; // Read 8-bit data from bus
                        if (addr & 0x01)
                        {
                            // Write to MSB
                            bank_registers[bank_index] = (bank_registers[bank_index] & 0x00FF) | (data << 8);
                        }
                        else
                        {
                            // Write to LSB
                            bank_registers[bank_index] = (bank_registers[bank_index] & 0xFF00) | data;
                        }

                        // Ensure reserved MSB bits are zero
                        bank_registers[bank_index] &= 0x0FFF;
                    }

                    while (!(gpio_get(PIN_WR))) // Wait for write cycle to complete
                    {
                        tight_loop_contents();
                    }
                }
            }
        }
    }
}

// -----------------------
// Main program
// -----------------------
int __no_inline_not_in_flash_func(main)()
{

    set_sys_clock_khz(150000, true);

    // Initialize stdio
    stdio_init_all();
    // Initialize GPIO
    setup_gpio();

    //setup_data_gpio();

    char rom_name[ROM_NAME_MAX];
    memcpy(rom_name, rom, ROM_NAME_MAX);
    uint8_t rom_type = rom[ROM_NAME_MAX];   // Get the ROM type

    uint32_t rom_size;
    memcpy(&rom_size, rom + ROM_NAME_MAX + 1, sizeof(uint32_t));

    // Print the ROM name and type
    printf("ROM name: %s\n", rom_name);
    printf("ROM type: %d\n", rom_type);
    printf("ROM size: %d\n", rom_size);

    // Load the ROM based on the detected type
    // 1 - 16KB ROM
    // 2 - 32KB ROM
    // 3 - Konami SCC ROM
    // 4 - 48KB Linear0 ROM
    // 5 - ASCII8 ROM
    // 6 - ASCII16 ROM
    // 7 - Konami (without SCC) ROM
    // 8 - NEO8 ROM
    switch (rom_type) 
    {
        case 1:
        case 2:
            //loadrom_plain32(0x1d); // flash version
            //loadrom_plain32_dma(0x1d); // dma version
            loadrom_plain32_pio(0x1d); // pio version
            //loadrom_plain32_sram(0x1d, rom_size); //sram version
            //loadrom_plain32_sram(pio, sm_addr, 0x1d, rom_size);
            //loadrom_plain32(pio, sm_addr, 0x1d);
            break;
        case 3:
            loadrom_konamiscc(0x1d); // flash version
            //loadrom_konamiscc(pio, sm_addr, 0x1d);
            //loadrom_konamiscc_sram(0x1d, rom_size);
            break;
        case 4:
            loadrom_linear48(0x1d); // flash version
            //loadrom_linear48_sram(0x1d, rom_size); //sram version
            break;
        case 5:
            loadrom_ascii8(0x1d); // flash version
            //loadrom_ascii8_sram(0x1d, rom_size); //sram version
            break;
        case 6:
            loadrom_ascii16(0x1d); //flash version
            //loadrom_ascii16_sram(0x1d, rom_size); //sram version
            break;
        case 7:
            loadrom_konami(0x1d); //flash version
            //loadrom_konami_sram(0x1d, rom_size); //sram version
            break;
        case 8:
            loadrom_neo8(0x1d); //flash version
            break;
        case 9:
            loadrom_neo16(0x1d); //flash version
            break;
        default:
            printf("Unknown ROM type: %d\n", 1);
            break;
    }


    return 0;
}
