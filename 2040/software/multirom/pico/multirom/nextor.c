// MSX PICOVERSE PROJECT
// (c) 2025 Cristiano Goncalves
// The Retro Hacker
//
// nextor.c - This is the Raspberry Pico firmware that will be used as a bridge between MSX NEXTOR and USB Mass Storage Devices (MSD).
//
// MSX NEXTOR bridge: the Pico speaks a simple control/data protocol while proxying USB MSC.
// Control port writes choose an action, data port transfers payloads, and TinyUSB performs
// the heavy lifting once the bus is idle. The state machine below keeps strict separation
// between MSX bus timing and asynchronous USB transactions.
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "bsp/board.h"
#include "tusb.h"
#include "class/msc/msc_host.h"

#include "multirom.h"
#include "nextor.h"

static scsi_inquiry_resp_t inquiry_resp; 

#define LANGUAGE_ID 0x0409 // English (United States)

volatile bool usb_device_info_valid = false; // Flag to indicate valid USB device info is in place
volatile bool usb_task_running = true; // Flag to indicate USB task is running

usb_device_info_t usb_device_info = {0}; // Cached USB device information
uint32_t usb_block_count = 0; // Cached block count
uint32_t usb_block_size = 0; // Cached block size

CFG_TUH_MEM_SECTION TU_ATTR_ALIGNED(4) static uint8_t block_read_buffer[512]; // Buffer for block read data
static volatile bool block_read_in_progress = false; // Flag to indicate a block read is in progress
static volatile bool block_read_ready = false; // Flag to indicate a block read is ready
static volatile bool block_read_failed = false; // Flag to indicate a block read has failed
static uint16_t block_read_length = 0; // Length of the last block read
static volatile uint32_t block_read_lba = 0; // LBA of the last block read
static uint8_t current_dev_addr = 0; // Current device address
static uint8_t current_lun = 0; // Current logical unit number
static volatile bool read_sequence_valid = false; // Flag to indicate a valid read sequence
static volatile uint32_t read_next_lba = 0; // Next LBA to read

CFG_TUH_MEM_SECTION TU_ATTR_ALIGNED(4) static uint8_t block_write_buffer[512]; // Buffer for block write data
static volatile bool block_write_in_progress = false; // Flag to indicate a block write is in progress
static volatile bool block_write_done = false; // Flag to indicate a block write is done
static volatile bool block_write_failed = false; // Flag to indicate a block write has failed
static volatile uint32_t block_write_lba = 0; // LBA of the last block write
static volatile bool write_sequence_valid = false; // Flag to indicate a valid write sequence
static volatile uint32_t write_next_lba = 0; // Next LBA to write

static bool start_block_read(uint32_t lba); 
static bool block_read_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const* cb_data);
static bool start_block_write(uint32_t lba);
static bool block_write_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const* cb_data);

// Callback invoked once the TinyUSB inquiry command completes.
bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const* cb_data) {

    msc_cbw_t const* cbw = cb_data->cbw;
    msc_csw_t const* csw = cb_data->csw;

    if (csw->status != 0)
    {
        printf("Inquiry failed\r\n");
        return false;
    }

    // Print out Vendor ID, Product ID and Rev
    //printf("%.8s %.16s rev %.4s\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev);

    // Get capacity of device
    usb_block_count = tuh_msc_get_block_count(dev_addr, cb_data->cbw->lun);
    //usb_block_count = 3932160; // For testing purposes, assume a fixed size
    //usb_block_count = 1000; // For testing purposes, assume a fixed size
    usb_block_size = tuh_msc_get_block_size(dev_addr, cb_data->cbw->lun);

    //printf("Disk Size: %lu MB\r\n", block_count / ((1024*1024)/block_size));
    //printf("Block Count = %lu, Block Size: %lu\r\n", block_count, block_size);

    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    //printf("VID: 0x%04X, PID: 0x%04X\r\n", vid, pid);

    memset(&usb_device_info, 0, sizeof(usb_device_info));
    memcpy(usb_device_info.vendor_id, inquiry_resp.vendor_id, sizeof(inquiry_resp.vendor_id));
    usb_device_info.vendor_id[sizeof(usb_device_info.vendor_id) - 1] = '\0';
    memcpy(usb_device_info.product_id, inquiry_resp.product_id, sizeof(inquiry_resp.product_id));
    usb_device_info.product_id[sizeof(usb_device_info.product_id) - 1] = '\0';
    memcpy(usb_device_info.product_rev, inquiry_resp.product_rev, sizeof(inquiry_resp.product_rev));
    usb_device_info.product_rev[sizeof(usb_device_info.product_rev) - 1] = '\0';
    usb_device_info.vid = vid;
    usb_device_info.pid = pid;
    
    usb_device_info_valid = true;
    usb_task_running = false;
    return true;
}

// Callback invoked when a MSC device is mounted.
void tuh_msc_mount_cb(uint8_t dev_addr)
{
    uint8_t const lun = 0;
    current_dev_addr = dev_addr;
    current_lun = lun;
    block_read_in_progress = false;
    block_read_ready = false;
    block_read_failed = false;
    read_sequence_valid = false;
    read_next_lba = 0;
    block_write_in_progress = false;
    block_write_done = false;
    block_write_failed = false;
    block_write_lba = 0;
    write_sequence_valid = false;
    write_next_lba = 0;
    usb_task_running = true;
    tuh_msc_inquiry(dev_addr, lun, &inquiry_resp, inquiry_complete_cb, 0);
}

// Callback invoked when a MSC device is unmounted.
void tuh_msc_umount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
    current_dev_addr = 0;
    current_lun = 0;
    usb_device_info_valid = false;
    memset(&usb_device_info, 0, sizeof(usb_device_info));
    block_read_in_progress = false;
    block_read_ready = false;
    block_read_failed = false;
    block_read_length = 0;
    block_read_lba = 0;
    read_sequence_valid = false;
    read_next_lba = 0;
    block_write_in_progress = false;
    block_write_done = false;
    block_write_failed = false;
    block_write_lba = 0;
    write_sequence_valid = false;
    write_next_lba = 0;
    usb_task_running = false;
}

// Start a block read operation for the specified LBA.
static bool block_read_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const* cb_data)
{
    (void)dev_addr;

    block_read_in_progress = false;
    usb_task_running = false;

    if (cb_data == NULL || cb_data->csw == NULL || cb_data->csw->status != 0)
    {
        block_read_ready = false;
        block_read_failed = true;
        block_read_length = 0;
        return false;
    }

    block_read_failed = false;
    block_read_length = usb_block_size;
    if (block_read_length > sizeof(block_read_buffer))
    {
        block_read_length = sizeof(block_read_buffer);
    }
    block_read_ready = true;
    return true;
}

// Start a block read operation for the specified LBA.
static bool start_block_read(uint32_t lba)
{
    if (!usb_device_info_valid || current_dev_addr == 0)
    {
        return false;
    }

    if (block_read_in_progress)
    {
        return false;
    }

    if (usb_block_size == 0 || usb_block_size > sizeof(block_read_buffer))
    {
        return false;
    }

    if (lba >= usb_block_count)
    {
        return false;
    }

    block_read_in_progress = true;
    block_read_ready = false;
    block_read_failed = false;
    block_read_length = 0;
    block_read_lba = lba;
    usb_task_running = true;

    if (!tuh_msc_read10(current_dev_addr,
                         current_lun,
                         block_read_buffer,
                         lba,
                         1,
                         block_read_complete_cb,
                         0))
    {
        block_read_in_progress = false;
        block_read_ready = false;
        block_read_failed = true;
        block_read_length = 0;
        usb_task_running = false;
        return false;
    }

    return true;
}

// Start a block write operation for the specified LBA.
static bool block_write_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const* cb_data)
{
    (void)dev_addr;

    block_write_in_progress = false;
    block_write_done = true;
    usb_task_running = false;

    if (cb_data == NULL || cb_data->csw == NULL || cb_data->csw->status != 0)
    {
        block_write_failed = true;
        write_sequence_valid = false;
        return false;
    }

    block_write_failed = false;
    write_sequence_valid = true;
    return true;
}

// Start a block write operation for the specified LBA.
static bool start_block_write(uint32_t lba)
{
    if (!usb_device_info_valid || current_dev_addr == 0)
    {
        return false;
    }

    if (block_write_in_progress)
    {
        return false;
    }

    if (usb_block_size == 0 || usb_block_size > sizeof(block_write_buffer))
    {
        return false;
    }

    if (lba >= usb_block_count)
    {
        return false;
    }

    block_write_in_progress = true;
    block_write_done = false;
    block_write_failed = false;
    block_write_lba = lba;
    usb_task_running = true;

    if (!tuh_msc_write10(current_dev_addr,
                         current_lun,
                         block_write_buffer,
                         lba,
                         1,
                         block_write_complete_cb,
                         0))
    {
        block_write_in_progress = false;
        block_write_done = false;
        block_write_failed = true;
        usb_task_running = false;
        write_sequence_valid = false;
        return false;
    }

    write_sequence_valid = true;
    write_next_lba = lba + 1;
    return true;
}

// Main I/O loop handling MSX NEXTOR control and data ports.
void __not_in_flash_func(nextor_io)() 
{
    uint8_t control_response = 0xFF;    // Latest status byte to feed back on control port reads
    uint8_t data_response_buffer[512];
    uint16_t data_response_length = 0;
    uint16_t data_response_index = 0;
    bool data_response_pending = false;      // Flag that indicates a fresh response is waiting
    bool read_address = false;               // Flag to indicate address read in progress
    uint8_t read_address_bytes[4] = {0};
    uint8_t read_address_index = 0;
    uint32_t latched_read_lba = 0;
    bool write_address = false;              // Flag to indicate write address collection in progress
    uint8_t write_address_bytes[4] = {0};
    uint8_t write_address_index = 0;
    bool write_data_pending = false;         // Flag that indicates we are collecting write payload bytes
    uint16_t write_data_index = 0;
    uint32_t pending_write_lba = 0;
    uint32_t current_write_lba = 0;
    
    bool usb_host_active = false;       // Set when TinyUSB reports host initialised
    
    memset(data_response_buffer, 0xFF, sizeof(data_response_buffer));
    
    while (true) {

        if (block_read_ready && !data_response_pending)
        {
            size_t payload = block_read_length;
            if (payload == 0 || payload > sizeof(block_read_buffer))
            {
                payload = usb_block_size;
            }

            if (payload == 0 || payload > sizeof(block_read_buffer))
            {
                // Device reported an unexpected size; flag error.
                block_read_ready = false;
                block_read_failed = true;
            }
            else
            {
                size_t transfer_len = payload;
                if (transfer_len > 512)
                {
                    transfer_len = 512;
                }

                memcpy(data_response_buffer, block_read_buffer, transfer_len);
                if (transfer_len < 512)
                {
                    memset(&data_response_buffer[transfer_len], 0x00, 512 - transfer_len);
                    transfer_len = 512;
                }

                data_response_length = (uint16_t)transfer_len;
                data_response_index = 0;
                data_response_pending = true;
                block_read_ready = false;
                control_response = 0x00;
            }
        }
        else if (block_read_failed)
        {
            block_read_failed = false;
            block_read_ready = false;
            data_response_pending = false;
            data_response_length = 0;
            data_response_index = 0;
            read_sequence_valid = false;
            read_next_lba = 0;
            control_response = 0xFF;
        }

        if (block_write_done)
        {
            bool success = !block_write_failed;
            block_write_done = false;
            if (!success)
            {
                control_response = 0xFF;
                write_data_pending = false;
                write_address = false;
                write_data_index = 0;
                block_write_failed = false;
            }
            else
            {
                control_response = 0x00;
                block_write_failed = false;
            }
        }

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
                if (port == PORT_CONTROL) // Port 0x9E (Control Write): Set the control register.
                {
                    if (busdata == 0x01) // Initialize USB Host
                    {

                        if (!usb_device_info_valid)
                        {
                            usb_task_running = true;

                            control_response = 0x01; // in progress
                            // init host stack on configured roothub port
                            if (!usb_host_active)
                            {
                                usb_host_active = tusb_init();
                            } 
                        
                            //board_init_after_tusb();
                            tuh_init(0);
                        }
                        
                        control_response = 0x00;
                    }
                    else if (busdata == 0x02) // Get USB VID (two reads, low byte then high byte)
                    {

                        if (usb_device_info_valid)
                        {
                            data_response_buffer[0] = (uint8_t)(usb_device_info.vid & 0xFF);
                            data_response_buffer[1] = (uint8_t)((usb_device_info.vid >> 8) & 0xFF);
                            data_response_length = 2;
                            data_response_index = 0;
                            data_response_pending = true;
                            control_response = 0x00;
                        }
                        else
                        {
                            control_response = 0xFF;
                        }
                    }
                    else if (busdata == 0x03) // Get USB Vendor ID, Product ID and Rev
                    {
                        if (usb_device_info_valid)
                        {
                            // Prepare response: Vendor ID (8 bytes), Product ID (16 bytes), Product Rev (4 bytes)
                            memset(data_response_buffer, 0xFF, sizeof(data_response_buffer));
                            size_t offset = 0;
                            size_t len = strlen(usb_device_info.vendor_id);
                            if (len > 8) len = 8;
                            memcpy(&data_response_buffer[offset], usb_device_info.vendor_id, len);
                            offset += 8;
                            len = strlen(usb_device_info.product_id);
                            if (len > 16) len = 16;
                            memcpy(&data_response_buffer[offset], usb_device_info.product_id, len);
                            offset += 16;
                            len = strlen(usb_device_info.product_rev);
                            if (len > 4) len = 4;
                            memcpy(&data_response_buffer[offset], usb_device_info.product_rev, len);
                            offset += 4; 

                            // Prepare response: Vendor ID (8 bytes), Product ID (16 bytes), Product Rev (4 bytes)

                            data_response_length = offset;
                            data_response_index = 0;
                            data_response_pending = true;
                            control_response = 0x00;
                        }
                        else
                        {
                            control_response = 0xFF;
                        }
                    }
                    else if (busdata == 0x04) // Get USB sectors (32 bits)
                        {
                            if (usb_device_info_valid)
                            {
                                uint32_t const block_count = usb_block_count; 
                                data_response_buffer[0] = (uint8_t)(block_count & 0xFF);
                                data_response_buffer[1] = (uint8_t)((block_count >> 8) & 0xFF);
                                data_response_buffer[2] = (uint8_t)((block_count >> 16) & 0xFF);
                                data_response_buffer[3] = (uint8_t)((block_count >> 24) & 0xFF);
                                data_response_length = 4;
                                data_response_index = 0;
                                data_response_pending = true;
                                control_response = 0x00;
                            }
                            else
                            {
                                control_response = 0xFF;
                            }
                        }
                    else if (busdata == 0x05) // Get USB block size (32 bits)
                        {
                            if (usb_device_info_valid)
                            {
                                uint32_t const block_size = usb_block_size; 
                                data_response_buffer[0] = (uint8_t)(block_size & 0xFF);
                                data_response_buffer[1] = (uint8_t)((block_size >> 8) & 0xFF);
                                data_response_buffer[2] = (uint8_t)((block_size >> 16) & 0xFF);
                                data_response_buffer[3] = (uint8_t)((block_size >> 24) & 0xFF);
                                data_response_length = 4;
                                data_response_index = 0;
                                data_response_pending = true;
                                control_response = 0x00;
                            }
                            else
                            {
                                control_response = 0xFF;
                            }
                        }

                    else if (busdata == 0x06) // Read a single 512-byte block
                    {
                        if (!usb_device_info_valid || usb_block_size == 0 || usb_block_size > sizeof(block_read_buffer))
                        {
                            control_response = 0xFF;
                        }
                        else if (data_response_pending || block_read_in_progress)
                        {
                            control_response = 0xFF;
                        }
                        else
                        {
                            block_read_ready = false;
                            block_read_failed = false;
                            read_sequence_valid = false;
                            read_next_lba = 0;
                            read_address = true;
                            read_address_index = 0;
                            memset(read_address_bytes, 0x00, sizeof(read_address_bytes));
                            data_response_pending = false;
                            data_response_length = 0;
                            data_response_index = 0;
                            latched_read_lba = 0;
                            control_response = 0x01; // Busy until block is ready
                        }
                    }
                    else if (busdata == 0x07) // Read next sequential 512-byte block
                    {
                        if (!usb_device_info_valid || !read_sequence_valid)
                        {
                            control_response = 0xFF;
                        }
                        else if (block_read_in_progress)
                        {
                            control_response = 0xFF;
                        }
                        else
                        {
                            uint32_t requested_lba = read_next_lba;
                            if (requested_lba >= usb_block_count)
                            {
                                control_response = 0xFF;
                                read_sequence_valid = false;
                            }
                            else if (!start_block_read(requested_lba))
                            {
                                control_response = 0xFF;
                                read_sequence_valid = false;
                            }
                            else
                            {
                                read_sequence_valid = true;
                                read_next_lba = requested_lba + 1;
                                control_response = 0x01; // Busy until block is ready
                            }
                        }
                    }
                    else if (busdata == 0x08) // Write a single 512-byte block with explicit address
                    {
                        if (!usb_device_info_valid || usb_block_size == 0 || usb_block_size > sizeof(block_write_buffer))
                        {
                            control_response = 0xFF;
                        }
                        else if (write_address || write_data_pending || block_write_in_progress)
                        {
                            control_response = 0xFF;
                        }
                        else
                        {
                            write_address = true;
                            write_address_index = 0;
                            memset(write_address_bytes, 0x00, sizeof(write_address_bytes));
                            write_data_pending = false;
                            write_data_index = 0;
                            pending_write_lba = 0;
                            write_sequence_valid = false;
                            write_next_lba = 0;
                            control_response = 0x01; // Busy while collecting address/data
                        }
                    }
                    else if (busdata == 0x09) // Write next sequential 512-byte block
                    {
                        if (!usb_device_info_valid || !write_sequence_valid)
                        {
                            control_response = 0xFF;
                        }
                        else if (write_address || write_data_pending || block_write_in_progress)
                        {
                            control_response = 0xFF;
                        }
                        else if (write_next_lba >= usb_block_count)
                        {
                            control_response = 0xFF;
                            write_sequence_valid = false;
                        }
                        else
                        {
                            pending_write_lba = write_next_lba;
                            current_write_lba = pending_write_lba;
                            write_data_pending = true;
                            write_data_index = 0;
                            control_response = 0x01; // Busy while collecting payload
                        }
                    }
                    else
                    {
                        control_response = 0xFF; // Unknown command
                    }
                }
                else if (port == PORT_DATAREG) // Port 0x9F (Data Write)
                {
                    if (read_address)
                    {
                        read_address_bytes[read_address_index++] = busdata;
                        if (read_address_index >= sizeof(read_address_bytes))
                        {
                            latched_read_lba = (uint32_t)read_address_bytes[0]
                                             | ((uint32_t)read_address_bytes[1] << 8)
                                             | ((uint32_t)read_address_bytes[2] << 16)
                                             | ((uint32_t)read_address_bytes[3] << 24);
                            read_address = false;
                            read_address_index = 0;

                            if (!start_block_read(latched_read_lba))
                            {
                                control_response = 0xFF;
                                read_sequence_valid = false;
                                read_next_lba = 0;
                            }
                            else
                            {
                                read_sequence_valid = true;
                                read_next_lba = latched_read_lba + 1;
                                control_response = 0x01; // Remain busy until callback completes
                            }
                        }
                    }
                    else if (write_address)
                    {
                        write_address_bytes[write_address_index++] = busdata;
                        if (write_address_index >= sizeof(write_address_bytes))
                        {
                            pending_write_lba = (uint32_t)write_address_bytes[0]
                                              | ((uint32_t)write_address_bytes[1] << 8)
                                              | ((uint32_t)write_address_bytes[2] << 16)
                                              | ((uint32_t)write_address_bytes[3] << 24);
                            write_address = false;
                            write_address_index = 0;

                            if (pending_write_lba >= usb_block_count)
                            {
                                control_response = 0xFF;
                                write_sequence_valid = false;
                                write_data_pending = false;
                                write_data_index = 0;
                            }
                            else
                            {
                                current_write_lba = pending_write_lba;
                                write_data_pending = true;
                                write_data_index = 0;
                            }
                        }
                    }
                    else if (write_data_pending)
                    {
                        size_t expected_bytes = usb_block_size;
                        if (expected_bytes == 0 || expected_bytes > sizeof(block_write_buffer))
                        {
                            control_response = 0xFF;
                            write_sequence_valid = false;
                            write_data_pending = false;
                            write_data_index = 0;
                        }
                        else
                        {
                            if (write_data_index < expected_bytes)
                            {
                                block_write_buffer[write_data_index++] = busdata;
                            }

                            if (write_data_index >= expected_bytes)
                            {
                                write_data_pending = false;
                                write_data_index = 0;
                                if (!start_block_write(current_write_lba))
                                {
                                    control_response = 0xFF;
                                }
                                else
                                {
                                    control_response = 0x01; // Remain busy until write completes
                                }
                            }
                        }
                    }
                }
                while (!gpio_get(PIN_WR)) tight_loop_contents();

            }
            else if (rd)
            {
                if (port == PORT_CONTROL) // Port 0x9E (Control Read): Return last status byte.
                {
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    gpio_put_masked(0xFF0000, control_response << 16); // Place data on data bus
                    while (!gpio_get(PIN_RD)) tight_loop_contents();
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode
                }
                else if (port == PORT_DATAREG) // Port 0x9F (Data Read): Return descriptor bytes when pending.
                {
                    uint8_t value = 0xFF;
                    if (data_response_pending && data_response_index < data_response_length)
                    {
                        value = data_response_buffer[data_response_index++];
                        if (data_response_index >= data_response_length)
                        {
                            data_response_pending = false;
                            data_response_length = 0;
                            data_response_index = 0;
                            control_response = 0x00;
                        }
                    }
                    gpio_set_dir_out_masked(0xFF << 16); // Set data bus to output mode
                    gpio_put_masked(0xFF0000, value << 16); // Place
                    while (!gpio_get(PIN_RD)) tight_loop_contents();
                    gpio_set_dir_in_masked(0xFF << 16); // Return data bus to input mode
                }
                while (!gpio_get(PIN_RD)) tight_loop_contents();

            }
        }
        // USB Host task handling
        // only run when a device is connected and no data response is pending
        // usb operations can take the CPU and affect timing critical operations on the IO bus
        if (usb_host_active && !data_response_pending && usb_task_running) {
            tuh_task(); // Keep the USB host stack running
        }
    }
}

