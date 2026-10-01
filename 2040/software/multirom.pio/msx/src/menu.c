// MSX PICOVERSE PROJECT
// (c) 2024 Cristiano Goncalves
// The Retro Hacker
//
// menu.c - MSX ROM with the menu program for the MSX PICOVERSE 2040 project
//
// This program will display a menu with the games stored on the flash memory. The user can navigate the menu using the arrow keys and select a game to load. 
// The program will display the game name, size and mapper type. The user can also display a help screen with the available keys.
// The program will read the flash memory configuration area to populate the game list. The configuration area will 
// contain the game name, size, mapper type and offset in the flash memory.
// 
// The program needs to be compiled using the Fusion-C library and the MSX BIOS routines. 
// 
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "menu.h"

#ifndef MULTIROM_VERSION
#define MULTIROM_VERSION "v1.00"
#endif

#define MENU_HLINE_SOURCE_CHAR 0x17
#define MENU_HLINE_PRINT_CHAR  0x7E
#define MENU_SEPARATOR_WIDTH   37
#define MENU_ROW_WIDTH         40
#define MENU_NAME_WIDTH        24
#define MENU_SIZE_COL          26
#define MENU_MAPPER_COL        31

static void copy_separator_char_pattern(void);
static void print_separator_line(void);
static void render_menu_row(unsigned int recordIndex, unsigned char row, unsigned char selected);
static int render_menu_row_scrolled(unsigned int recordIndex, unsigned char row, int startPos);
static void clear_menu_row(unsigned char row);
static void render_menu_page(void);

// Physical column where the BIOS places logical column 0. The BIOS centers the
// text window when LINLEN < 40 (e.g. WIDTH 37 on the Philips VG-8020), so the
// direct VRAM row writes must apply the same offset to line up with CHPUT text.
unsigned char menu_col_offset;

static void detect_menu_col_offset(void)
{
    unsigned int base = *(unsigned int *)BIOS_TXTNAM;

    menu_col_offset = 0;
    for (unsigned char i = 0; i < 8; i++) {
        if (Vpeek(base + i) != ' ') {
            menu_col_offset = i;
            break;
        }
    }
}

static void blit_row_vram(unsigned char row, const char *src) __naked
{
    (void)row;
    (void)src;
    __asm
    ld      hl, #2
    add     hl, sp
    ld      b, (hl)        ; B = row
    inc     hl
    ld      e, (hl)        ; src low
    inc     hl
    ld      d, (hl)        ; DE = src

    push    de

    ld      de, #MENU_ROW_WIDTH
    ld      hl, #0
    ld      a, b
    or      a
    jr      z, blit_addr_done

blit_addr_loop:
    add     hl, de
    dec     a
    jr      nz, blit_addr_loop

blit_addr_done:
    ld      de, (#BIOS_TXTNAM)
    add     hl, de
    ld      a, (_menu_col_offset)
    ld      e, a
    ld      d, #0
    add     hl, de         ; skip the BIOS left margin
    ex      de, hl         ; DE = destination VRAM address
    pop     hl             ; HL = source RAM address
    neg
    add     a, #MENU_ROW_WIDTH
    ld      c, a
    ld      b, #0          ; BC = MENU_ROW_WIDTH - offset (stay on this line)

    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_LDIRVM
    call    BIOS_CALSLT
    pop     ix
    ret
    __endasm;
}

// read_ulong - Read a 4-byte value from the memory area
// This function will read a 4-byte value from the memory area pointed by ptr and return the value as an unsigned long
// Parameters:
//   ptr - Pointer to the memory area to read the value from
// Returns:
//   The 4-byte value as an unsigned long 
unsigned long read_ulong(const unsigned char *ptr) {
    return (unsigned long)ptr[0] |
           ((unsigned long)ptr[1] << 8) |
           ((unsigned long)ptr[2] << 16) |
           ((unsigned long)ptr[3] << 24);
}

// isEndOfData - Check if the memory area is the end of the data
// This function will check if the memory area pointed by memory is the end of the data. The end of the data is defined by all bytes being 0xFF.
// Parameters:
//   memory - Pointer to the memory area to check
// Returns:
//   1 if the memory area is the end of the data, 0 otherwise
int isEndOfData(const unsigned char *memory) {
    for (int i = 0; i < ROM_RECORD_SIZE; i++) {
        if (memory[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

// readROMData - Read the ROM records from the memory area
// This function will read the ROM records from the memory area pointed by memory and store them in the records array. The function will stop reading
// when it reaches the end of the data or the maximum number of records is reached.
// Parameters:
//   records - Pointer to the array of ROM records to store the data
//   recordCount - Pointer to the variable to store the number of records read
void readROMData(ROMRecord *records, unsigned char *recordCount, unsigned long *sizeTotal) {
    unsigned char *memory = (unsigned char *)MEMORY_START;
    unsigned char count;
    unsigned long total;

    count = 0;
    total = 0;
    while (count < MAX_ROM_RECORDS && !isEndOfData(memory)) {
        // Copy Name
        MemCopy(records[count].Name, memory, MAX_FILE_NAME_LENGTH);
        // The name field in flash is a fixed 50-byte record and may not be
        // null-terminated. Scan only within bounds to avoid memory corruption.
        unsigned int name_len = 0;
        while (name_len < MAX_FILE_NAME_LENGTH && records[count].Name[name_len] != '\0') {
            name_len++;
        }

        // pad with spaces in case of short names
        for (unsigned int i = name_len; i < MAX_FILE_NAME_LENGTH; i++) {
            records[count].Name[i] = ' ';
        }
        // Ensure null termination
        records[count].Name[MAX_FILE_NAME_LENGTH] = '\0'; // Ensure null termination
        records[count].Mapper = memory[MAX_FILE_NAME_LENGTH]; // Read Mapper code
        records[count].Size = read_ulong(&memory[MAX_FILE_NAME_LENGTH + 1]); // Read Size (4 bytes)
        records[count].Offset = read_ulong(&memory[MAX_FILE_NAME_LENGTH + 5]); // Read Offset (4 bytes)
        memory += ROM_RECORD_SIZE; // Move to the next record
        total += records[count].Size;
        count++;
    }

    *recordCount = count;
    *sizeTotal = total;
}


void wait1s() {
    unsigned int start = *(unsigned int*)JIFFY;
    while ((unsigned int)(*(unsigned int*)JIFFY - start) < 60);
}

// putchar - Print a character on the screen
// This function will override the putchar function to use the MSX BIOS routine to print characters on the screen
// This is to deal with the mess we have between the Fusion-C putchar and the SDCC Z80 library putchar
// Parameters:
//  character - The character to print
// Returns:
//  The character printed
int putchar (int character)
{
    __asm
    ld      hl, #2              ;Get the return address of the function
    add     hl, sp              ;Bypass the return address of the function 
    ld      a, (hl)              ;Get the character to print

    ld      iy,(#BIOS_EXPTBL-1)  ;BIOS slot in iyh
    push    ix                     ;save ix
    ld      ix,#BIOS_CHPUT       ;address of BIOS routine
    call    BIOS_CALSLT          ;interslot call
    pop ix                      ;restore ix
    __endasm;

    return character;
}

void clear_screen_0() {
    __asm
    ld      a, #0            ; Set SCREEN 0 mode
    call    BIOS_CHGMOD    ; Call BIOS CHGMOD to change screen mode to SCREEN 0
    call    BIOS_CLS       ; Call BIOS CLS function to clear the screen
    __endasm;
}

void clear_fkeys()
{
    __asm
    ld hl, #BIOS_FNKSTR    ; Load the starting address of function key strings into HL
    ld de, #0xF880    ; Load the next address into DE for block fill
    ld bc, #160       ; Set BC to 160, the number of bytes to clear
    ld (hl), #0       ; Initialize the first byte to 0

clear_loop:
    ldi              ; Load (HL) with (DE), increment HL and DE, decrement BC
    dec hl           ; Adjust HL back to the correct position
    ld (hl), #0      ; Set the current byte to 0
    inc hl           ; Move to the next byte
    dec bc           ; Decrement the byte counter
    ld a, b          ; Check if BC has reached zero
    or c
    jp nz, clear_loop ; Repeat until all bytes are cleared
    __endasm;
}

static void copy_separator_char_pattern(void)
{
    unsigned int sourceAddress = 0x0800 + ((unsigned int)MENU_HLINE_SOURCE_CHAR * 8);
    unsigned int targetAddress = 0x0800 + ((unsigned int)MENU_HLINE_PRINT_CHAR * 8);

    for (unsigned char i = 0; i < 8; i++) {
        Vpoke(targetAddress + i, Vpeek(sourceAddress + i));
    }
}

static void print_separator_line(void)
{
    for (unsigned char i = 0; i < MENU_SEPARATOR_WIDTH; i++) {
        PrintChar(MENU_HLINE_PRINT_CHAR);
    }
}

static void fill_menu_row(char *buffer)
{
    for (unsigned char i = 0; i < MENU_ROW_WIDTH; i++) {
        buffer[i] = ' ';
    }
}

static void copy_menu_text(char *dst, const char *src, unsigned char width, unsigned char inverted)
{
    for (unsigned char i = 0; i < width; i++) {
        unsigned char ch = src[i] ? (unsigned char)src[i] : ' ';
        dst[i] = (char)(inverted ? (unsigned char)(ch + 96) : ch);
    }
}

static int copy_menu_text_window(char *dst, const char *src, size_t len, int startPos)
{
    int hasVisible = 0;
    int base = startPos % (int)len;

    if (base < 0) {
        base += (int)len;
    }

    for (unsigned char i = 0; i < MENU_NAME_WIDTH; i++) {
        unsigned char ch = (unsigned char)src[(base + (int)i) % (int)len];
        dst[i] = (char)(ch + 96);
        if (ch != ' ') {
            hasVisible = 1;
        }
    }

    return hasVisible;
}

static void write_menu_size(char *dst, unsigned int value)
{
    dst[0] = (char)('0' + ((value / 1000U) % 10U));
    dst[1] = (char)('0' + ((value / 100U) % 10U));
    dst[2] = (char)('0' + ((value / 10U) % 10U));
    dst[3] = (char)('0' + (value % 10U));
}

static void clear_menu_row(unsigned char row)
{
    char buffer[MENU_ROW_WIDTH];
    fill_menu_row(buffer);
    blit_row_vram(row, buffer);
}

static void render_menu_row(unsigned int recordIndex, unsigned char row, unsigned char selected)
{
    char buffer[MENU_ROW_WIDTH];
    const char *mapper;
    unsigned int size_kb;

    fill_menu_row(buffer);
    buffer[0] = selected ? '>' : ' ';
    copy_menu_text(&buffer[1], records[recordIndex].Name, MENU_NAME_WIDTH, selected);

    size_kb = (unsigned int)(records[recordIndex].Size / 1024UL);
    write_menu_size(&buffer[MENU_SIZE_COL], size_kb);

    mapper = mapper_description(records[recordIndex].Mapper);
    copy_menu_text(&buffer[MENU_MAPPER_COL], mapper, 6, 0);

    blit_row_vram(row, buffer);
}

static int render_menu_row_scrolled(unsigned int recordIndex, unsigned char row, int startPos)
{
    char buffer[MENU_ROW_WIDTH];
    const char *mapper;
    size_t len = strlen(records[recordIndex].Name);
    unsigned int size_kb;
    int hasVisible;

    if (len == 0) {
        render_menu_row(recordIndex, row, 1);
        return 0;
    }

    if (len <= MENU_NAME_WIDTH) {
        render_menu_row(recordIndex, row, 1);
        return 1;
    }

    fill_menu_row(buffer);
    buffer[0] = '>';
    hasVisible = copy_menu_text_window(&buffer[1], records[recordIndex].Name, len, startPos);
    if (!hasVisible) {
        return 0;
    }

    size_kb = (unsigned int)(records[recordIndex].Size / 1024UL);
    write_menu_size(&buffer[MENU_SIZE_COL], size_kb);

    mapper = mapper_description(records[recordIndex].Mapper);
    copy_menu_text(&buffer[MENU_MAPPER_COL], mapper, 6, 0);

    blit_row_vram(row, buffer);
    return 1;
}

// invert_chars - Invert the characters in the character table
// This function will invert the characters from startChar to endChar in the character table. We use it to copy and invert the characters from the
// normal character table area to the inverted character table area. This is to display the game names in the inverted character table.
// Parameters:
//   startChar - The first character to invert
//   endChar - The last character to invert
void invert_chars(unsigned char startChar, unsigned char endChar)
{
    unsigned int srcAddress, dstAddress;
    unsigned char patternByte;
    unsigned char i, c;

    for (c = startChar; c <= endChar; c++)
    {
        // Each character has 8 bytes in the pattern table.
        srcAddress  = 0x0800 + ((unsigned int)c * 8);
        // Calculate destination address (shift by +95 bytes)
        dstAddress = srcAddress + (96*8);

        // Flip all 8 bytes that define this character.
        for (i = 0; i < 8; i++)
        {
            patternByte = Vpeek(srcAddress + i);
            patternByte = ~patternByte;           // CPL (bitwise NOT)
            Vpoke(dstAddress  + i, patternByte);
        }
    }
}

// print_str_inverted - Print a string using the inverted character table
// This function will print a string using the inverted character table. It will apply an offset to the characters to display them correctly.
// Used to display the game names in the inverted characters.
// Parameters:
//   str - The string to print
void print_str_inverted(const char *str) 
{
    char buffer[25];
    size_t len = strlen(str);

    if (len > 24) {
        len = 24; // keep on-screen width stable
    }

    memcpy(buffer, str, len);

    for (size_t i = len; i < 24; i++) {
        buffer[i] = ' ';
    }

    buffer[24] = '\0';

    for (size_t i = 0; i < 24; i++) {
        int modifiedChar = buffer[i] + 96; // Apply the offset to the character
        PrintChar(modifiedChar); // Print the modified character
    }

}

// print_str_inverted_sliding - Print a sliding string using the inverted character table
// This function will print a sliding string using the inverted character table. It will apply an offset
int print_str_inverted_sliding(const char *str, int startPos) 
{
    size_t len = strlen(str);

    if (len == 0) {
        for (size_t i = 0; i < 24; i++) {
            PrintChar(' ' + 96); // Keep the highlighted row blank when no name is present
        }
        return 0;
    }

    if (len <= 24) {
        print_str_inverted(str);
        return 1;
    }

    int base = startPos % (int)len;
    if (base < 0) {
        base += (int)len;
    }

    unsigned char window[24];
    int hasVisible = 0;

    for (size_t i = 0; i < 24; i++) {
        unsigned char ch = (unsigned char)str[(base + (int)i) % (int)len];
        window[i] = ch;
        if (ch != ' ') {
            hasVisible = 1;
        }
    }

    if (!hasVisible) {
        return 0;
    }

    for (size_t i = 0; i < 24; i++) {
        PrintChar((int)window[i] + 96); // Apply the offset to the character
    }
    return 1;
}

// bios_chsns - Check if a key has been pressed
// This function will check if a key has been pressed using the MSX BIOS routine CHSNS
static unsigned char bios_chsns(void) __naked
{
    __asm
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_CHSNS
    call    BIOS_CALSLT
    pop     ix
    jr      z, bios_chsns_no_key
    ld      l,a
    ld      h,#0
    ret

bios_chsns_no_key:
    xor     a
    ld      l,a
    ld      h,a
    ret
    __endasm;
}

// bios_chget - Get a character from the keyboard buffer
// This function will get a character from the keyboard buffer using the MSX BIOS routine.
static unsigned char bios_chget(void) __naked
{
    __asm
    ld      iy,(#BIOS_EXPTBL-1)
    push    ix
    ld      ix,#BIOS_CHGET
    call    BIOS_CALSLT
    pop     ix
    ld      l,a
    ld      h,#0
    ret
    __endasm;
}

// joystick_direction_to_key - Convert joystick direction to key code
// This function will convert the joystick direction to the corresponding key code.
static int joystick_direction_to_key(unsigned char dir)
{
    switch (dir) {
        case 1:
            return 30;
        case 3:
            return 28;
        case 5:
            return 31;
        case 7:
            return 29;
        default:
            return 0;
    }
}

// wait_for_key_with_scroll - Wait for a key press with scrolling effect
// This function will wait for a key press and return the key code. While waiting, it
static int wait_for_key_with_scroll(unsigned int recordIndex, unsigned int row)
{
    volatile unsigned int *jiffyPtr = (volatile unsigned int *)JIFFY;
    unsigned int lastTick = *jiffyPtr;
    int startPos = 0;
    const char *name = records[recordIndex].Name;
    size_t len = strlen(name);
    int shouldScroll = (len > 24);
    const unsigned int scrollDelay = 30U; // 0.5 seconds at 60 Hz

    while (1)
    {
        
        // check for key press
        unsigned char peek = bios_chsns();
        if (peek) {
            return (int)bios_chget();
        }

        /*
        //joystick is too fast and not precise for menu navigation
        //need to implement some kind of delay or repeat rate control
        //also Fusion-C triggers are not working properly, need to investigate further

        // check for joystick input
        unsigned char joy1 = JoystickRead(0x01);
        unsigned char joy2 = JoystickRead(0x02);
        
        // any joystick inputs are valid
        int joyKey1 = joystick_direction_to_key(joy1); 
        if (joyKey1) {
            return joyKey1;
        }
        int joyKey2 = joystick_direction_to_key(joy2);
        if (joyKey2) {
            return joyKey2;
        }

        int joyButton1 = TriggerRead(0x01);
        int joyButton2 = TriggerRead(0x02);
        int joyButton = joyButton1 | joyButton2;
        if (joyButton) {
            return 13; // Enter key
        }
        */

        //debug
        //Locate(0, 23);
        //printf("Joy1: %02X Joy2: %02X", joy1, joy2);
        //printf("TR1: %02X TR2: %02X", joyButton1, joyButton2);

        // handle scrolling
        if (shouldScroll) {
            unsigned int now = *jiffyPtr; // read current jiffy count
            if ((unsigned int)(now - lastTick) >= scrollDelay) {
                int attempts = 0;
                int printed = 0;
                int lenInt = (int)len;

                while (attempts < lenInt && !printed) {
                    printed = render_menu_row_scrolled(recordIndex, (unsigned char)row, startPos);
                    startPos++;
                    if (startPos >= lenInt) {
                        startPos = 0;
                    }
                    attempts++;
                }
                lastTick = now;
            }
        }


    }

    


    
}

// mapper_description - Get the description of the mapper type
// This function will return the description of the mapper type based on the mapper number.
const char* mapper_description(int number) {
    switch (number) {
        case 1:  return "PLA-16";
        case 2:  return "PLA-32";
        case 3:  return "KNSCC";
        case 4:  return "PLN-48";
        case 5:  return "ASC08";
        case 6:  return "ASC16";
        case 7:  return "KONAMI";
        case 8:  return "NEO-8";
        case 9:  return "NEO16";
        case 10: return "SYSTEM";
        case 11: return "SYSTEM";
        case 12: return "ASC16X";
        case 13: return "PLN-64";
        case 14: return "MANBW2";
        default: return "UNKWN";
    }
}

// displayMenu - Display the menu on the screen
// This function will display the menu on the screen. It will print the header, the files on the current page and the footer with the page number and options.
void displayMenu() {
    //Cls(); // for some reason is not working here
    //clear_screen_0(); // works but reset the char table
    Screen(0);
    copy_separator_char_pattern();
    invert_chars(32, 126); // Invert the characters from 32 to 126
    //FunctionKeys(1); // Disable the function keys


    Locate(0, 0);
    printf("MSX PICOVERSE 2040   [MultiROM %s]", MULTIROM_VERSION);
    detect_menu_col_offset();
    Locate(0, 1);
    print_separator_line();
    render_menu_page();
    Locate(0, 21);
    print_separator_line();
}

static void render_menu_page(void) {
    unsigned int startIndex = (currentPage - 1) * FILES_PER_PAGE;
    unsigned int endIndex = startIndex + FILES_PER_PAGE;

    if (endIndex > totalFiles) {
        endIndex = totalFiles;
    }

    unsigned int line = 0;
    for (unsigned int idx = startIndex; idx < endIndex; idx++, line++) {
        render_menu_row(idx, (unsigned char)(2 + line), (unsigned char)(idx == currentIndex));
    }

    for (; line < FILES_PER_PAGE; line++) {
        clear_menu_row((unsigned char)(2 + line));
    }

    Locate(0, 22);
    printf("Page: %02d/%02d                [H - Help]",currentPage, totalPages); // Print the page number and the help option
}

// helpMenu - Display the help menu on the screen
// This function will display the help menu on the screen. It will print the help information and the keys to navigate the menu.
void helpMenu()
{
    
    Cls(); // Clear the screen
    copy_separator_char_pattern();
    Locate(0,0);
    printf("MSX PICOVERSE 2040   [MultiROM %s]", MULTIROM_VERSION);
    Locate(0, 1);
    print_separator_line();
    Locate(0, 2);
    printf("Use [UP]  [DOWN] to navigate the menu");
    Locate(0, 3);
    printf("Use [LEFT] [RIGHT] to navigate  pages");
    Locate(0, 4);
    printf("Press [ENTER] or [SPACE] to load the ");
    Locate(0, 5);
    printf("  selected rom file");
    Locate(0, 6);
    printf("Press [H] to display the help screen");
    Locate(0, 21);
    print_separator_line();
    Locate(0, 22);
    printf("Press any key to return to the menu!");
    InputChar();
    displayMenu();
    navigateMenu();
}


void msx_warm_reboot(void)
{
    __asm
        di
        rst  0x0000    
    __endasm;
}

// msx_cold_reboot - Disable warm boot on MSX2+ by manipulating port F4
// This forces a cold boot on the next reset, making the MSX show the full startup screen.
// Based on the Carnivore2 reset sequence.
void msx_cold_reboot(void)
{
    __asm
        di
        in	    a,(#0xF4)		; read from F4 port on MSX2+
	    or	    #0x80         ; set bit 7 to disable warm boot
	    out	    (#0xF4),a		; avoid "warm" reset on MSX2+
        rst     0x0000    
    __endasm;
}

// loadGame - Load the game from the flash memory
// This function will load the game from the flash memory based on the index. 
// For Nextor/SYSTEM ROMs (mapper 10 and 11), it performs a Carnivore2-style cold reboot
// that forces the MSX to show the full startup screen.
void loadGame(int index) 
{
    if (records[index].Mapper != 0)
    {
        Poke(ROM_SELECT_REGISTER, index); // Set the game index

        // For Nextor/SYSTEM ROMs, perform a cold reboot (Carnivore2 style)
        if (records[index].Mapper == 10 || records[index].Mapper == 11)
        {
            msx_cold_reboot();
        }
        else msx_warm_reboot(); // Execute RST 00h to reset the MSX computer and load the game
    }
}

// navigateMenu - Navigate the menu
// This function will navigate the menu. It will wait for the user to press a key and then act based on the key pressed. The user can navigate the menu using the arrow keys
// to move up and down the files, left and right to move between pages, enter to load the game, and H to display the help screen.
// The function will update the current page and current index based on the key pressed and display the menu again.
void navigateMenu() 
{
    char key;

    while (1) 
    {
        //debug
        Locate(0, 23);
        unsigned int currentRow = (currentIndex%FILES_PER_PAGE) + 2;
        unsigned int previousIndex = currentIndex;
        unsigned int previousRow = currentRow;
        int pageRedrawn = 0;

        key = wait_for_key_with_scroll(currentIndex, currentRow);
        //key = KeyboardRead();
        //key = InputChar();
        char fkey = Fkeys();
        (void)fkey;

        switch (key) 
        {
            case 30: // Up arrow
                if (currentIndex > 0) // Check if we are not at the first file
                {
                    if (currentIndex%FILES_PER_PAGE >= 0) currentIndex--; // Move to the previous file
                    if (currentIndex < ((currentPage-1) * FILES_PER_PAGE))  // Check if we need to move to the previous page
                    {
                        currentPage--; // Move to the previous page
                        render_menu_page(); // Redraw only page rows and footer
                        pageRedrawn = 1;
                    }
                }
                break;
            case 31: // Down arrow
                if ((currentIndex%FILES_PER_PAGE < FILES_PER_PAGE) && currentIndex < totalFiles-1) currentIndex++; // Move to the next file
                if (currentIndex >= (currentPage * FILES_PER_PAGE)) // Check if we need to move to the next page
                {
                    currentPage++; // Move to the next page
                    render_menu_page(); // Redraw only page rows and footer
                    pageRedrawn = 1;
                }
                break;
            case 28: // Right arrow
                if (currentPage < totalPages) // Check if we are not on the last page
                {
                    currentPage++; // Move to the next page
                    currentIndex = (currentPage-1) * FILES_PER_PAGE; // Move to the first file of the page
                    render_menu_page(); // Redraw only page rows and footer
                    pageRedrawn = 1;
                }
                break;
            case 29: // Left arrow
                if (currentPage > 1) // Check if we are not on the first page
                {
                    currentPage--; // Move to the previous page
                    currentIndex = (currentPage-1) * FILES_PER_PAGE; // Move to the first file of the page
                    render_menu_page(); // Redraw only page rows and footer
                    pageRedrawn = 1;
                }
                break;
            case 27: // ESC
                // load Nextor
                //loadGame(0); // Load the Nextor ROM
                break;
            case 72: // H - Help (uppercase H)
            case 104: // h - Help (lowercase h)
                // Help
                helpMenu(); // Display the help menu
                break;
            case 13: // Enter
            case 32: // Space
                // Load the game
                loadGame(currentIndex); // Load the selected game
                break;
        }
        if (!pageRedrawn && currentIndex != previousIndex) {
            render_menu_row(previousIndex, (unsigned char)previousRow, 0);
            render_menu_row(currentIndex, (unsigned char)((currentIndex%FILES_PER_PAGE) + 2), 1);
        }
        Locate(0, (currentIndex%FILES_PER_PAGE) + 2); // Position the cursor on the selected file
    }
}

void main() {
    // Initialize the variables
    currentPage = 1; // Start on page 1
    currentIndex = 0; // Start at the first file - index 0
    
    readROMData(records, &totalFiles, &totalSize);
    totalPages = (int)((totalFiles/FILES_PER_PAGE)+1); // Calculate the total pages based on the total files and files per page

    //Screen(0); // Set the screen mode
    //invert_chars(32, 126); // Invert the characters from 32 to 126
    clear_fkeys(); // Clear the function keys
    //KillKeyBuffer(); // Clear the key buffer

    // Display the menu
    displayMenu();
    // Activate navigation
    navigateMenu();
}