// MSX PICOVERSE PROJECT
// (c) 2024 Cristiano Goncalves
// The Retro Hacker
//
// menu.c - MSX ROM with the menu program for the MSX PICOVERSE 2350 project
//
// This program will display a menu with the games stored on the flash memory. The user can navigate the menu using the arrow keys and select a game to load. 
// The program will display the game name, size and mapper type. The user can also display a help screen with the available keys and a configuration screen 
// to change the settings of the program. The program will read the flash memory configuration area to populate the game list. The configuration area will 
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
        // pad with spaces in case of short names
        for (int i = strlen(records[count].Name); i < MAX_FILE_NAME_LENGTH; i++) {
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

// execute_rst00 - Execute the RST 00h instruction to reset the MSX computer
void execute_rst00() {
    __asm
        rst 0x00
    __endasm;
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
static int wait_for_key_with_scroll(const char *name, unsigned int row)
{
    volatile unsigned int *jiffyPtr = (volatile unsigned int *)JIFFY;
    unsigned int lastTick = *jiffyPtr;
    int startPos = 0;
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
                    Locate(1, row);
                    printed = print_str_inverted_sliding(name, startPos);
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
char* mapper_description(int number) {
    // Array of strings for the descriptions
    const char *descriptions[] = {"PL-16", "PL-32", "KonSCC", "Linear", "ASC-08", "ASC-16", "Konami","NEO-8","NEO-16","SYSTEM"};	
    return descriptions[number - 1];
}

// displayMenu - Display the menu on the screen
// This function will display the menu on the screen. It will print the header, the files on the current page and the footer with the page number and options.
void displayMenu() {
    //Cls(); // for some reason is not working here
    //clear_screen_0(); // works but reset the char table
    Screen(0);
    invert_chars(32, 126); // Invert the characters from 32 to 126
    //FunctionKeys(1); // Disable the function keys


    Locate(0, 0);
    printf("MSX PICOVERSE 2350   [MultiROM %s]", MULTIROM_VERSION);
    Locate(0, 1);
    printf("-------------------------------------");
    unsigned int startIndex = (currentPage - 1) * FILES_PER_PAGE;
    unsigned int endIndex = startIndex + FILES_PER_PAGE;

    if (endIndex > totalFiles) {
        endIndex = totalFiles;
    }

    unsigned int line = 0;
    for (unsigned int idx = startIndex; idx < endIndex; idx++, line++) {
        Locate(0, 2 + line); // Position on the screen, starting at line 2
        printf(" %-24.24s %04lu %-7s", records[idx].Name, records[idx].Size/1024, mapper_description(records[idx].Mapper));
    }

    for (; line < FILES_PER_PAGE; line++) {
        Locate(0, 2 + line);
        printf("                                 "); // clear unused lines to avoid stale entries
    }
    // footer
    Locate(0, 21);
    printf("-------------------------------------");
    Locate(0, 22);
    printf("Page: %02d/%02d                [H - Help]",currentPage, totalPages); // Print the page number and the help and config options
    if (totalFiles > 0) {
        Locate(0, (currentIndex % FILES_PER_PAGE) + 2); // Position the cursor on the selected file
        printf(">"); // Print the cursor
        print_str_inverted(records[currentIndex].Name); // Print the selected file name inverted

    }
}

// configMenu - Display the configuration menu on the screen
// This function will display the configuration menu on the screen. 
void configMenu()
{
    Cls(); // Clear the screen
    Locate(0,0);
    printf("MSX PICOVERSE 2350   [MultiROM %s]", MULTIROM_VERSION);
    Locate(0, 1);
    printf("-------------------------------------");
    Locate(0, 2);
    Locate(0, 21);
    printf("-------------------------------------");
    Locate(0, 22);
    printf("Press any key to return to the menu!");
    InputChar();
    displayMenu();
    navigateMenu();
}

// helpMenu - Display the help menu on the screen
// This function will display the help menu on the screen. It will print the help information and the keys to navigate the menu.
void helpMenu()
{
    
    Cls(); // Clear the screen
    Locate(0,0);
    printf("MSX PICOVERSE 2350   [MultiROM %s]", MULTIROM_VERSION);
    Locate(0, 1);
    printf("-------------------------------------");
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
    printf("-------------------------------------");
    Locate(0, 22);
    printf("Press any key to return to the menu!");
    InputChar();
    displayMenu();
    navigateMenu();
}

// loadGame - Load the game from the flash memory
// This function will load the game from the flash memory based on the index. 
void loadGame(int index) 
{
    if (records[index].Mapper != 0)
    {
        Poke(ROM_SELECT_REGISTER, index); // Set the game index
        execute_rst00(); // Execute RST 00h to reset the MSX computer and load the game
        execute_rst00();
    }
}

// navigateMenu - Navigate the menu
// This function will navigate the menu. It will wait for the user to press a key and then act based on the key pressed. The user can navigate the menu using the arrow keys
// to move up and down the files, left and right to move between pages, enter to load the game, H to display the help screen and C to display the config screen.
// The function will update the current page and current index based on the key pressed and display the menu again.
void navigateMenu() 
{
    char key;

    while (1) 
    {
        //debug
        Locate(0, 23);
        //printf("Key: %3d", key);
        //printf("Size: %05lu/15872", totalSize/1024);
        //debug
        //Locate(20, 23);
        //printf("Memory Mapper: Off");
        //printf("CPage: %2d Index: %2d", currentPage, currentIndex);
        unsigned int currentRow = (currentIndex%FILES_PER_PAGE) + 2;

        key = wait_for_key_with_scroll(records[currentIndex].Name, currentRow);
        //key = KeyboardRead();
        //key = InputChar();
        char fkey = Fkeys();
        (void)fkey;

        Locate(0, currentRow); // Position the cursor on the previously selected file
        printf(" "); // Clear the cursor
        printf("%-24.24s", records[currentIndex].Name); // Print only the first 24 characters of the file name
        switch (key) 
        {
            case 30: // Up arrow
                if (currentIndex > 0) // Check if we are not at the first file
                {
                    if (currentIndex%FILES_PER_PAGE >= 0) currentIndex--; // Move to the previous file
                    if (currentIndex < ((currentPage-1) * FILES_PER_PAGE))  // Check if we need to move to the previous page
                    {
                        currentPage--; // Move to the previous page
                        displayMenu(); // Display the menu
                    }
                }
                break;
            case 31: // Down arrow
                if ((currentIndex%FILES_PER_PAGE < FILES_PER_PAGE) && currentIndex < totalFiles-1) currentIndex++; // Move to the next file
                if (currentIndex >= (currentPage * FILES_PER_PAGE)) // Check if we need to move to the next page
                {
                    currentPage++; // Move to the next page
                    displayMenu(); // Display the menu
                }
                break;
            case 28: // Right arrow
                if (currentPage < totalPages) // Check if we are not on the last page
                {
                    currentPage++; // Move to the next page
                    currentIndex = (currentPage-1) * FILES_PER_PAGE; // Move to the first file of the page
                    displayMenu(); // Display the menu
                }
                break;
            case 29: // Left arrow
                if (currentPage > 1) // Check if we are not on the first page
                {
                    currentPage--; // Move to the previous page
                    currentIndex = (currentPage-1) * FILES_PER_PAGE; // Move to the first file of the page
                    displayMenu(); // Display the menu
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
            case 99: // C - Config (uppercase C)
            case 67: // c - Config (lowercase c)
                // Config
                configMenu(); // Display the config menu
                break;
            case 13: // Enter
            case 32: // Space
                // Load the game
                loadGame(currentIndex); // Load the selected game
                break;
        }
        Locate(0, (currentIndex%FILES_PER_PAGE) + 2); // Position the cursor on the selected file
        printf(">"); // Print the cursor
        print_str_inverted(records[currentIndex].Name); // Print the selected file name
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