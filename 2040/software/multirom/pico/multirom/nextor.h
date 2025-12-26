#include <stdbool.h>
#include <stdint.h>

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

void __not_in_flash_func(nextor_io)();