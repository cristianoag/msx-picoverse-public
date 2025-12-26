// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// nextor.h - This is the Raspberry Pico firmware that will be used as a bridge between MSX NEXTOR and Mass Storage Devices (SD Card and USB).
//
// MSX NEXTOR bridge: the Pico speaks a simple control/data protocol while proxying SDCard/USB MSC.
// Control port writes choose an action, data port transfers payloads, and appropriate libraries perform
// the heavy lifting once the bus is idle. The state machine below keeps strict separation
// between MSX bus timing and asynchronous disk (sd and USB) transactions.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#define PORT_CONTROL   0x9E //PORTCFG 
#define PORT_DATAREG   0x9F //PORTSPI

typedef struct {
	char vendor_id[9];
	char product_id[17];
	char product_rev[5];
	uint16_t vid;
	uint16_t pid;
} usb_device_info_t;

extern volatile bool usb_device_info_valid;
extern usb_device_info_t usb_device_info;

void __not_in_flash_func(nextor_sd_io)();
void __not_in_flash_func(nextor_usb_io)();