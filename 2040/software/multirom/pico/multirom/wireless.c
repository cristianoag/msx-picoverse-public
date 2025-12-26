// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// wireless.c - This is the wireless implementation for the MSX PICOVERSE 2040 project
//
// This firmware is responsible for loading the multirom menu and the ROMs selected by the user into the MSX. When flashed through the 
// multirom tool, it will be stored on the pico flash memory followed by the MSX MENU ROM (with the config) and all the ROMs processed by the 
// multirom tool. The sofware in this firmware will load the first 32KB ROM that contains the menu into the MSX and it will allow the user
// to select a ROM to be loaded into the MSX. The selected ROM will be loaded into the MSX and the MSX will be reseted to run the selected ROM.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "multirom.h"
#include "wireless.h"

// protocol documentation available at: https://github.com/cristianoag/msx-picoverse/wiki/ESP8266-Module-for-Wireless

uint8_t port_f2_value = 0xFF; // Default value for port F2


//TESTING THE UART
void __not_in_flash_func(wireless_main_test)() {

    // Set GPIO28 and GPIO29 to UART function
    gpio_set_function(28, GPIO_FUNC_UART); // TX
    gpio_set_function(29, GPIO_FUNC_UART); // RX

    uart_init(uart0, 859372 ); // Initialize UART with a baud rate of 859372
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE); // Set UART format: 8 data bits, 1 stop bit, no parity
    uart_set_hw_flow(uart0, false, false); // Disable hardware flow control

    sleep_ms(10000); // Wait for 2 seconds
    printf("UART initialized.\n"); // Print message to console
    while (true) {
        printf("Sending 55 to the UART: \n"); // Print message to UART
        uint8_t data = 55; // Test data to send
        uart_putc(uart0, data); // Send message over UART
        printf("Waiting for 2 seconds...\n"); // Print message to console
        sleep_ms(2000);
        printf("Reading from the UART: \n"); // Print message to console
        //while(uart_is_readable(uart0)) { // Check if data is available to read
            uint8_t c = uart_getc(uart0); // Read character from UART
            printf("%d",c); 
        //}
        printf("\n"); // Print newline to console
    }
}


void __not_in_flash_func(wireless_main)(){

    // Set GPIO28 and GPIO29 to UART function
    gpio_set_function(UART0_TX, GPIO_FUNC_UART); // TX
    gpio_set_function(UART0_RX, GPIO_FUNC_UART); // RX

    uart_init(uart0, 859372 ); // Initialize UART with a baud rate of 859372
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE); // Set UART format: 8 data bits, 1 stop bit, no parity
    uart_set_hw_flow(uart0, false, false); // Disable hardware flow control
    //uart_set_fifo_enabled(uart0, false); // Disable FIFO
    //uart_set_irq_enables(uart0, false, false); // Disable UART interrupts
    //uart_set_irq_enables(uart0, true, false); // Enable RX interrupts
    //uart_set_irq_enables(uart0, false, true); // Enable TX interrupts
    //uart_set_irq_enables(uart0, false, false); // Disable TX interrupts
    //uart_set_irq_enables(uart0, false, false); // Disable RX interrupts

    while (true) {
        bool iorq  = !gpio_get(PIN_IORQ);
        if (iorq)
        { 
            uint32_t gpiostates = gpio_get_all();
            uint8_t busdata = (gpiostates >> 16) & 0xFF;
            uint8_t port = gpiostates & 0xFF;

            bool wr = !gpio_get(PIN_WR);
            bool rd = !gpio_get(PIN_RD);
            if (wr)
            {
                
                if (port == 0xF2) { // PORT 0xF2 write cycle
                    printf("0xF2 CMD: %02X\n", busdata);
                    port_f2_value = busdata; // Store the value written to port F2
                } else if (port == 0x06) { // PORT 0x06 write cycle
                    //uart_puts(uart0, "Hello, UART0 on GPIO28 and GPIO29!\n");
                    //Send commands to the UART
                    //printf("0x06 CMD UART0 TX: %02X\n", busdata);
                    uint8_t data = busdata; // Read the data from the UART
                    uart_putc(uart0, data); // Send the data to the UART
                    //printf("0x06 DATA UART0 TX: %02X\n", data);
                } else if (port == 0x07) { // PORT 0x07 write cycle
                    //Send data to the UART
                    //uart_puts(uart0, "Hello, UART0 on GPIO28 and GPIO29!\n");
                    uint8_t data = busdata; // Read the data from the UART
                    //printf("0x07 DATA UART0 TX: %02X\n", data);
                    uart_putc(uart0, data); // Send the data to the UART
                } 

                while (!gpio_get(PIN_WR)) tight_loop_contents();

            }
            else if (rd)
            {
                // PORT 0xF2 read cycle
                if (port == 0xF2) {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    gpio_put_masked(0xFF0000, port_f2_value << 16); // Write the value of port F2 to the data bus
                    while (!(gpio_get(PIN_RD))) { // Wait until the read cycle completes (RD goes high)
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode
                } else if (port == 0x06) { // PORT 0x06 read cycle

                    //uint8_t data = uart_getc(uart0); // Read the data from the UART
                    uint8_t data = 0x11;
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    gpio_put_masked(0xFF0000, data << 16); // Write the value of port 
                    while (!(gpio_get(PIN_RD))) { // Wait until the read cycle completes (RD goes high)
                        tight_loop_contents();
                    }
                    gpio_set_dir_in_masked(0xFF << 16); // Set data bus to input mode 
                } 
                else if (port == 0x07) { // PORT 0x07 read cycle

                    //uint8_t data = uart_getc(uart0); // Read the data from the UART
                    uint8_t data = 11;
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    gpio_put_masked(0xFF0000, data << 16); // Write the data to the data bus
                    while (!gpio_get(PIN_RD)) tight_loop_contents();
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode after cycle completes
                }
                
                while (!gpio_get(PIN_RD)) tight_loop_contents();

            }
        }
    }
}