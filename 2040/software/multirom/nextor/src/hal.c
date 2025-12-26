#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "hal.h"
#include "bios.h"

#ifdef DEBUG
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#endif

#define wait_time_usb 40

__at (BIOS_HWVER) uint8_t msx_version;
__at (BIOS_LINL40) uint8_t text_columns;

bool supports_80_column_mode()
{
    return msx_version>=1;
}

void hal_init ()
{
    text_columns = 40;
    // check MSX version
    if (supports_80_column_mode())
        text_columns = 80;

    __asm
    ld     iy,(#BIOS_EXPTBL-1)       ;BIOS slot in iyh
    push ix
    ld     ix,#BIOS_INITXT           ;address of BIOS routine
    call   BIOS_CALSLT               ;interslot call
    pop ix
    __endasm;    
}

void hal_deinit ()
{
    // nothing to do
}



uint8_t read_ctrl() __z88dk_fastcall __naked
{
    __asm
        ld a,(0x7B00)
        ld l,a
        ret
    __endasm;
}

uint8_t read_mnft() __z88dk_fastcall __naked
{
    __asm
        ld a,(0x7B01)
        ld l,a
        ret
    __endasm;
}

uint8_t read_8bit_value(uint16_t address) {
    uint8_t value = 0;
    uint8_t *ptr = (uint8_t *)address;
    value = *ptr;
    return value;
}

uint32_t read_32bit_value(uint16_t address) {
    uint32_t value = 0;
    uint8_t *ptr = (uint8_t *)address;

    value |= (uint32_t)ptr[0] << 24;
    value |= (uint32_t)ptr[1] << 16;
    value |= (uint32_t)ptr[2] << 8;
    value |= (uint32_t)ptr[3];

    return value;
}

void write_8bit_value(uint16_t address, uint8_t value) {
    uint8_t *ptr = (uint8_t *)address;
    *ptr = value;
}

void write_32bit_value(uint16_t address, uint32_t value) {
    uint8_t *ptr = (uint8_t *)address;

    ptr[0] = (value >> 24) & 0xFF;
    ptr[1] = (value >> 16) & 0xFF;
    ptr[2] = (value >> 8) & 0xFF;
    ptr[3] = value & 0xFF;
}

#pragma disable_warning 85    
void write_command (uint8_t command)  __z88dk_fastcall __naked
{
    __asm
        ld a,l
        out (#CMD_PORT),a
        ret
    __endasm;
}

#pragma disable_warning 85
void write_data (uint8_t data)  __z88dk_fastcall __naked
{
    __asm
        ld a,l
        out (#DATA_PORT),a 
        ret
    __endasm;
}

uint8_t read_data ()  __z88dk_fastcall __naked
{
    __asm
        in a,(#DATA_PORT) 
        ld l,a
        ret
    __endasm;
}

uint8_t read_status ()  __z88dk_fastcall __naked
{
    __asm
        in a,(#CMD_PORT) 
        ld l,a
        ret
    __endasm;
}

#pragma disable_warning 85
void  read_data_multiple (uint8_t* buffer,uint8_t len)
{
    __asm
    ld iy, #2
    add iy,sp
    ld b,+2(iy)
    ld h,+1(iy)
    ld l,+0(iy)
    ld c, #DATA_PORT
    .db 0xED,0xB2 ;inir 
    __endasm;
}

void write_data_multiple (uint8_t* buffer,uint8_t len)
{
    __asm
    ld iy, #2
    add iy,sp
    ld b,+2(iy)
    ld h,+1(iy)
    ld l,+0(iy)
    ld c, #DATA_PORT
    .db 0xED,0xB3 ;otir 
    __endasm;
}

#pragma disable_warning 85	// because the var msg is not used in C context
void msx_wait (uint16_t times_jiffy)  __z88dk_fastcall __naked
{
    __asm
    ei
    ; Wait a determined number of interrupts
    ; Input: BC = number of 1/framerate interrupts to wait
    ; Output: (none)
    WAIT:
        halt        ; waits 1/50th or 1/60th of a second till next interrupt
        dec hl
        ld a,h
        or l
        jr nz, WAIT
        ret

    __endasm; 
}

void delay_ms (uint16_t milliseconds) 
{
    msx_wait (milliseconds/20);
}

uint16_t getManufacturerID() 
{
    uint16_t manufacturer_id = 0;
    uint8_t status_attempts = 0;
    bool status_ready = false;

#ifdef DEBUG
        DEBUG_PRINT("getManufacturerID: ");
#endif
    write_command(0x02);
    //wait until status ready
    for (uint8_t max_wait = 20; max_wait > 0; max_wait--) {
#ifdef DEBUG
        DEBUG_PRINT(".");
#endif
        if (read_status() == 0x00) {
            status_ready = true;
            break;
        }
        delay_ms(wait_time_usb);
    }

    if (!status_ready) {
        printf("Error: Could not read Manufacturer ID!\r\n");
        manufacturer_id = 0xFFFF;
    } else {
        uint8_t id_low = read_data(); 
        uint8_t id_high = read_data();
        //printf("MAN. ID read (HL): %02X%02X\r\n", id_high, id_low);
        manufacturer_id = ((uint16_t)id_high << 8) | id_low;
        //printf("MAN ID (conv): %04X\r\n", manufacturer_id);
    }
        
    return manufacturer_id;
}

char* getManufacturerName(uint16_t manufacturer_id) 
{
    uint8_t status_attempts = 0;
    bool status_ready = false;
    char name_buffer[28];

    // try to get the string from the device
    //write_command(0x03);

    //wait until status ready
    do {
        write_command(0x03);
        delay_ms(100);
        status_ready = (read_status() == 0x00);
        status_attempts++;
    } while (!status_ready && status_attempts < 20);


    if (!status_ready) {
        printf("Error: Could not read Manufacturer String!\r\n");
        strncpy(name_buffer, "Error: Unknown USB Device", 28 );
    } else {
        // read the manufacturer name chars (28 bytes) and convert to string
        // read each byte and convert to char
        //for (uint8_t i = 0; i < 28; i++) {
        //    name_buffer[i] = read_data();
            //printf("%c", name_buffer[i]);
        //}
        //printf("\r\n");
        read_data_multiple((uint8_t *)name_buffer, 28);
        name_buffer[27] = '\0';
    }

    //delay_ms(5000);
    //return the manufacturer name (name_buffer);
    return name_buffer;
    

    // in case we cannot get the string from the device, return based on known IDs
/*     switch (manufacturer_id) {
        case 0x9FFF:
            return "Generic USB Mass Storage";
        default:
            return "Unknown USB Device";
    } */
}

uint32_t getCapacity() 
{
    uint32_t sd_capacity = 0;
    uint8_t status_attempts = 0;
    bool status_ready = false;

    do {
        write_command(0x04);
        delay_ms(100);
        status_ready = (read_status() == 0x00);
        status_attempts++;
        
    } while (!status_ready && status_attempts < 10);

    for (uint8_t i = 0; i < 4; i++)
    {
        uint8_t byte = read_data();
        sd_capacity |= (uint32_t)byte << (i * 8);
    }

    //printf("Capacity: %lu sectors\r\n", sd_capacity);
    return sd_capacity;
}

uint32_t getBlockSize() 
{
    uint32_t sd_block_size = 0;
    uint8_t status_attempts = 0;
    bool status_ready = false;

    do {
        write_command(0x05);
        delay_ms(100);
        status_ready = (read_status() == 0x00);
        status_attempts++;
    } while (!status_ready && status_attempts < 10);

    for (uint8_t i = 0; i < 4; i++)
    {
        uint8_t byte = read_data();
        sd_block_size |= (uint32_t)byte << (i * 8);
    }

    //printf("Block Size: %lu bytes\r\n", sd_block_size);
    return sd_block_size;
}

uint32_t getSerial() 
{
    uint32_t sd_serial = 0x82335178;
    return sd_serial;
}

bool read_write_disk_sectors (bool writing,uint8_t nr_sectors,uint32_t* sector,uint8_t* sector_buffer)
{
    if (!writing)
    {
        if (!sd_disk_read (nr_sectors,(uint8_t*)sector,sector_buffer))
            return false;
    }
    else
    {
        if (!sd_disk_write (nr_sectors,(uint8_t*)sector,sector_buffer))
            return false;
    }
    
    return true;
}

bool sd_disk_read (uint8_t nr_sectors,uint8_t* lba,uint8_t* sector_buffer)
{
    uint8_t nr = nr_sectors;
    uint16_t chunk;
    uint8_t sectors_read = 0;
    bool status_ready = false;

#ifdef DEBUG
    const uint32_t lba_value = ((uint32_t)lba[3] << 24) | ((uint32_t)lba[2] << 16) | ((uint32_t)lba[1] << 8) | lba[0];
    DEBUG_PRINT("USB read  LBA=%02X%02X%02X%02X: sectors=%u : ", lba[3],lba[2],lba[1],lba[0], nr_sectors);
#endif

    write_command(0x06);
    write_data(lba[0]);
    write_data(lba[1]);
    write_data(lba[2]);
    write_data(lba[3]);

    write_command(0x06);
    // wait for first sector to be ready
    for (uint8_t max_wait = 20; max_wait > 0; max_wait--) {    
#ifdef DEBUG
        DEBUG_PRINT(".");
#endif
        if (read_status() == 0x00) {
            status_ready = true;
            break;
        }
        delay_ms(wait_time_usb);
    }

    chunk = 0;
    while (chunk < 512) {
        read_data_multiple(sector_buffer, 32);
        sector_buffer += 32;
        chunk += 32;
    }

#ifdef DEBUG
    sectors_read++;
    DEBUG_PRINT("%u ", sectors_read);
#endif

    while (nr > 1) {
        write_command(0x07);
        for (uint8_t max_wait = 20; max_wait > 0; max_wait--) {
#ifdef DEBUG
            DEBUG_PRINT(".");
#endif
            if (read_status() == 0x00) {
                status_ready = true;
                break;
            }
            delay_ms(wait_time_usb);
        }
        chunk = 0;
        while (chunk < 512) {
            read_data_multiple(sector_buffer, 32);
            sector_buffer += 32;
            chunk += 32;
        }

#ifdef DEBUG
        sectors_read++;
        DEBUG_PRINT("%u ", sectors_read);
#endif
        nr--;
    }

#ifdef DEBUG
       DEBUG_PRINT("\r\n");
#endif

    return status_ready;
}

bool sd_disk_write(uint8_t nr_sectors,uint8_t* lba,uint8_t* sector_buffer)
{

    uint8_t nr = nr_sectors;
    uint16_t chunk;
    uint8_t sectors_wrote = 0;
    bool status_ready = false;


#ifdef DEBUG
    const uint32_t lba_value = ((uint32_t)lba[3] << 24) | ((uint32_t)lba[2] << 16) | ((uint32_t)lba[1] << 8) | lba[0];
    DEBUG_PRINT("USB write LBA=%02X%02X%02X%02X: sectors=%u : ", lba[3],lba[2],lba[1],lba[0], nr_sectors);
#endif

    //delay_ms(50);
    write_command(0x08);
    //delay_ms(40);
    write_data (lba[0]);
    write_data (lba[1]);
    write_data (lba[2]);
    write_data (lba[3]);
    //delay_ms(40);
    write_command(0x08);

    chunk = 0;
    while (chunk < 512) {
            write_data_multiple(sector_buffer, 32);
            sector_buffer += 32;
            chunk += 32;
    }

    // wait for write to complete
    for (uint8_t max_wait = 20; max_wait > 0; max_wait--) {
#ifdef DEBUG
        DEBUG_PRINT(".");
#endif
        if (read_status() == 0x00) {
            status_ready = true;
            break;
        }
        delay_ms(wait_time_usb);
    }

#ifdef DEBUG
        sectors_wrote++;
        DEBUG_PRINT("%u ", sectors_wrote);
#endif

    while (nr > 1) {
        write_command(0x09);
        chunk = 0;
        while (chunk < 512) {
            write_data_multiple(sector_buffer, 32);
            sector_buffer += 32;
            chunk += 32;
        }
        for (uint8_t max_wait = 20; max_wait > 0; max_wait--) {
#ifdef DEBUG
            DEBUG_PRINT(".");
#endif
            if (read_status() == 0x00) {
                status_ready = true;
                break;
            }
            delay_ms(wait_time_usb);
        }
#ifdef DEBUG
        sectors_wrote++;
        DEBUG_PRINT("%u ", sectors_wrote);
#endif
        nr--;
    }

#ifdef DEBUG
       DEBUG_PRINT("\r\n");
#endif

    return status_ready;
}



