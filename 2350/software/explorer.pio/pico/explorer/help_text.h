// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// help_text.h - F5 help pages served to the MSX menu by the Explorer firmware
//
// The menu ROM has no room for long texts, so it asks the firmware for one page
// at a time (CMD_HELP_PAGE) and prints the lines it finds in the data window.
// A page is a list of NUL separated lines; a line starting with '\1' is a
// heading, drawn inverted. Every page fits the 19 rows the menu has between the
// title and the footer (rows 2 to 20) and must not contain an empty line, which
// ends the page. The 40 and 80 column sets have the same pages in the same
// order: the menu opens the page that matches the screen the user came from.
//
// License: Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef EXPLORER_HELP_TEXT_H
#define EXPLORER_HELP_TEXT_H

#include <stdint.h>

#define HELP_PAGE_COUNT 6u
#define HELP_PAGE_ABOUT 5u // The firmware adds its version and chip ID to this page
#define HELP_MAX_ROWS 19u
#define HELP_WIDTH_40 39u
#define HELP_WIDTH_80 78u

#define HELP_H(text) "\1" text "\0"
#define HELP_L(text) text "\0"

typedef struct {
    const char *text; // Lines separated by NUL; the last one ends at the literal's NUL
    uint16_t size;    // Bytes in text, including that last NUL
} help_page_t;

#define HELP_PAGE(text) { text, (uint16_t)sizeof(text) }

// ---------------------------------------------------------------------------
// 80 columns
// ---------------------------------------------------------------------------

static const char help80_browse[] =
    HELP_H("Browsing the lists")
    HELP_L("UP / DOWN       Move the selection; the list moves to the next page by itself.")
    HELP_L("LEFT / RIGHT    Previous / next page of the list.")
    HELP_L("ENTER           Open the entry: a folder, the MP3 player or the ROM screen.")
    HELP_L("SPACE / FIRE    Quick-run the ROM with its saved options; the joystick")
    HELP_L("                fire button A or B does the same. On a folder it opens it.")
    HELP_L("ESC             Leave the current folder (the same as the .. entry).")
    HELP_L("/               Search: type part of a name, ENTER jumps to a match.")
    HELP_L("C               Switch between 40 and 80 columns on machines that have both.")
    HELP_L("F5              Show this help from any screen.")
    HELP_H("Sources")
    HELP_L("F1  Flash       ROMs kept in the cartridge; no microSD card needed.")
    HELP_L("F2  microSD     Folders, ROM, DSK, MP3 and WAV files of the memory card.")
    HELP_L("F3  FileHunter  Online ROM and disk catalog; needs the ESP-01 WiFi module.")
    HELP_L("F4  WiFi        Set the WiFi network and password; the MSX restarts.")
    HELP_H("Joystick")
    HELP_L("Directions move and change pages; fire button A or B quick-runs. It only works")
    "in the flash and microSD lists: everything else needs the keyboard.";

static const char help80_files[] =
    HELP_H("File commands (F2 microSD list)")
    HELP_L("D   Delete the selected file after a Y/N question; its .PVC options go too.")
    HELP_L("    In the F1 list it removes the selected flash entry. Folders are protected.")
    HELP_L("R   Rename a ROM, DSK image or flash entry. The mapper tag and extension stay.")
    HELP_L("    ENTER confirms, ESC cancels. Long names are limited by the screen width.")
    HELP_L("W   Create a folder in the folder you are browsing, on the current partition.")
    HELP_L("M   Move a file: press M on it, open the destination folder, press M again.")
    HELP_L("    Its .PVC options go along. ESC in the root, F1 or F3 cancel the move.")
    HELP_L("F   Copy the selected ROM into the cartridge flash after a Y/N question.")
    HELP_L("    Flash ROMs run without the card; their options are kept on the card.")
    HELP_L("P   Cycle through the FAT16, FAT32 and exFAT partitions of the card.")
    HELP_L("A   Make the folder you are browsing the File Hunter download folder.")
    HELP_H("Good to know")
    HELP_L("- A ROM larger than 4 MB has to be copied to flash before it can run.")
    HELP_L("- A ROM's options (mapper, audio, PSG...) are saved next to it as NAME.PVC.")
    HELP_L("- A tag in the file name forces the mapper: Knight Mare.PLA-32.ROM.")
    HELP_L("- Only .ROM, 360/720 KB .DSK, .MP3 and .WAV files are listed; folders first.")
    "- The bottom line shows the page and the free space of the flash or the card.";

static const char help80_rom[] =
    HELP_H("ROM screen (ENTER on a ROM)")
    HELP_L("UP / DOWN       Choose an option. The last row, Run, starts the ROM.")
    HELP_L("LEFT / RIGHT    Change the value of the chosen option.")
    HELP_L("ENTER / SPACE   Run the ROM (on the Run row).        ESC   Back to the list.")
    HELP_H("Options")
    HELP_L("Mapper    How the cartridge maps the ROM. SD ROMs are detected; change it only")
    HELP_L("          if the game does not start. ASC16X-FR adds FlashROM game saves.")
    HELP_L("Audio     Sound hardware emulated for the game: SCC, SCC+, MSX-MUSIC, Dual")
    HELP_L("          PSG, SFG01/SFG05, SN76489 Franky or an external SCC cartridge.")
    HELP_L("PSG       Yes copies the MSX PSG to the cartridge audio output (DAC).")
    HELP_L("Volume    Level of the cartridge audio output; 100% is the normal level.")
    HELP_L("Freq/CPU  50/60 Hz on MSX2+ and the CPU speed, when the computer allows it.")
    HELP_L("WiFi      Nextor entries only: expose the WiFi BIOS before booting.")
    HELP_L("SD Part   Nextor SD entries only: the FAT16 partition Nextor boots from.")
    HELP_H("Saved options")
    HELP_L("Run saves the options as NAME.PVC next to the ROM (flash: /NAME.flash.PVC).")
    HELP_L("SPACE in the list runs the ROM with those options. Delete the .PVC to reset.")
    HELP_H("Disk images")
    "A .DSK image boots through the built-in Nextor; only some options apply to it.";

static const char help80_mp3[] =
    HELP_H("MP3 / WAV player (ENTER on an audio file)")
    HELP_L("Plays the file from the microSD card through the cartridge audio output.")
    HELP_L("UP / DOWN       Choose Play/Stop, Pause/Resume or the play mode.")
    HELP_L("ENTER / SPACE   Run the chosen action.")
    HELP_L("LEFT / RIGHT    With the play mode chosen: Single, All or Random.")
    HELP_L("N / P           Next / previous song (All and Random modes, while playing).")
    HELP_L("ESC             Stop the music and go back to the list.")
    HELP_L("C               Switch between 40 and 80 columns.")
    HELP_H("Good to know")
    HELP_L("- Only the elapsed time is shown: the file is streamed, its length is unknown.")
    HELP_L("- In All and Random mode the player goes on to the next song by itself.")
    "- The music stops when you leave the player, change source or start a ROM.";

static const char help80_hunter[] =
    HELP_H("File Hunter (F3)")
    HELP_L("Online catalog of MSX ROMs and disk images. It needs the ESP-01 WiFi module,")
    HELP_L("set up with F4 (network name and password; the MSX restarts afterwards).")
    HELP_H("Result list")
    HELP_L("UP / DOWN       Move in the results.       LEFT / RIGHT   Change page.")
    HELP_L("ENTER / SPACE   Open the detail screen of the selected ROM or disk.")
    HELP_L("/               Search the catalog; ENTER runs the search, ESC cancels it.")
    HELP_L("T               Switch between the ROM and the DSK catalog (keeps the search).")
    HELP_L("A               Choose the download folder: browse the card, press A there.")
    HELP_L("C               Switch between 40 and 80 columns.")
    HELP_L("ESC             Back to the Explorer list.   F1 / F2   Flash / microSD list.")
    HELP_H("Detail screen")
    HELP_L("ENTER / SPACE   Download the file into the download folder (root by default).")
    HELP_L("A               Choose the download folder (shown on this screen).")
    HELP_L("ESC             Back to the result list.")
    "Open the F2 list afterwards to run what you downloaded.";

static const char help80_about[] =
    HELP_H("About Explorer")
    HELP_L("PicoVerse 2350 Explorer for MSX, by Cristiano Goncalves (The Retro Hacker).")
    HELP_L("Project: https://github.com/cristianoag/msx-picoverse-public")
    HELP_H("If something goes wrong")
    HELP_L("- No microSD card: reseat it. FAT16, FAT32 and exFAT cards are supported.")
    HELP_L("- A game does not start: try another mapper or audio option on the ROM screen.")
    HELP_L("- Unknown mapper: add a tag to the file name, for example Game.PLA-32.ROM.")
    HELP_L("- Odd options or sound: delete the NAME.PVC file next to the ROM.")
    HELP_L("- File Hunter offline: set the WiFi network again with F4.")
    "- More details are in the Explorer manual, in the docs folder of the project.";

static const help_page_t help_pages_80[HELP_PAGE_COUNT] = {
    HELP_PAGE(help80_browse), HELP_PAGE(help80_files), HELP_PAGE(help80_rom),
    HELP_PAGE(help80_mp3), HELP_PAGE(help80_hunter), HELP_PAGE(help80_about),
};

// ---------------------------------------------------------------------------
// 40 columns
// ---------------------------------------------------------------------------

static const char help40_browse[] =
    HELP_H("Browsing the lists")
    HELP_L("UP/DOWN     Move the selection")
    HELP_L("LEFT/RIGHT  Change page")
    HELP_L("ENTER       Open folder, player or ROM")
    HELP_L("SPACE/FIRE  Run the ROM with its saved")
    HELP_L("            options (folders: open)")
    HELP_L("ESC         Leave the folder")
    HELP_L("/           Search part of a name")
    HELP_L("C           Toggle 40/80 columns")
    HELP_L("F5          This help")
    HELP_H("Sources")
    HELP_L("F1 Flash    ROMs in the cartridge")
    HELP_L("F2 SD       Card folders and files")
    HELP_L("F3 Hunter   Online ROM/disk catalog")
    HELP_L("F4 WiFi     Network setup (restarts)")
    HELP_H("Joystick")
    "Directions move, fire runs (lists only)";

static const char help40_files[] =
    HELP_H("File commands (F2 list)")
    HELP_L("D  Delete (Y/N), also its .PVC")
    HELP_L("   In F1 it removes the flash entry")
    HELP_L("R  Rename; tag and extension stay")
    HELP_L("W  New folder here")
    HELP_L("M  Move: press M on a file, open the")
    HELP_L("   target folder, press M again")
    HELP_L("   (ESC in root, F1, F3 cancel)")
    HELP_L("F  Copy the ROM to flash (Y/N)")
    HELP_L("P  Next partition (FAT16/32/exFAT)")
    HELP_L("A  Make this the File Hunter folder")
    HELP_H("Good to know")
    HELP_L("ROMs over 4 MB must go to flash.")
    HELP_L("Options are saved as NAME.PVC")
    HELP_L("next to the ROM.")
    HELP_L("A tag in the name sets the mapper:")
    "Knight Mare.PLA-32.ROM";

static const char help40_rom[] =
    HELP_H("ROM screen (ENTER on a ROM)")
    HELP_L("UP/DOWN     Choose an option")
    HELP_L("LEFT/RIGHT  Change its value")
    HELP_L("ENTER       Run (on the Run row)")
    HELP_L("ESC         Back")
    HELP_H("Options")
    HELP_L("Mapper   Detected; change it if the")
    HELP_L("         game does not start")
    HELP_L("Audio    SCC, SCC+, MSX-MUSIC, Dual")
    HELP_L("         PSG, SFG, Franky...")
    HELP_L("PSG      Copy the PSG to the DAC")
    HELP_L("Volume   Cartridge audio level")
    HELP_L("Freq/CPU 50/60 Hz, CPU speed if any")
    HELP_L("WiFi     Nextor entries only")
    HELP_L("SD Part  Nextor SD entries only")
    HELP_H("Saved options")
    HELP_L("Run saves them as NAME.PVC; SPACE in")
    "the list runs the ROM with them.";

static const char help40_mp3[] =
    HELP_H("MP3/WAV player")
    HELP_L("Plays from the card through the")
    HELP_L("cartridge audio output.")
    HELP_L("UP/DOWN     Play, Pause or Mode")
    HELP_L("ENTER       Run the chosen action")
    HELP_L("LEFT/RIGHT  Mode: Single/All/Random")
    HELP_L("N / P       Next / previous song")
    HELP_L("            (All/Random, playing)")
    HELP_L("ESC         Stop and go back")
    HELP_L("C           Toggle 40/80 columns")
    HELP_H("Good to know")
    HELP_L("Only the elapsed time is shown.")
    "Leaving the player stops the music.";

static const char help40_hunter[] =
    HELP_H("File Hunter (F3)")
    HELP_L("Online ROM and disk catalog. Needs")
    HELP_L("the ESP-01 WiFi module (set up: F4).")
    HELP_H("Result list")
    HELP_L("UP/DOWN     Move")
    HELP_L("LEFT/RIGHT  Change page")
    HELP_L("ENTER       Open the detail screen")
    HELP_L("/           Search the catalog")
    HELP_L("T           Switch ROM / DSK list")
    HELP_L("A           Choose the download folder")
    HELP_L("ESC         Back to Explorer")
    HELP_L("F1/F2       Go to flash / SD list")
    HELP_H("Detail screen")
    HELP_L("ENTER/SPACE Download to the folder")
    HELP_L("A           Choose the download folder")
    HELP_L("ESC         Back to the list")
    "Open F2 afterwards to run the file.";

static const char help40_about[] =
    HELP_H("About Explorer")
    HELP_L("PicoVerse 2350 Explorer for MSX")
    HELP_L("by Cristiano Goncalves")
    HELP_L("github.com/cristianoag/")
    HELP_L("msx-picoverse-public")
    HELP_H("If something goes wrong")
    HELP_L("No card: reseat it (FAT16/32/exFAT)")
    HELP_L("Game fails: try another mapper or")
    HELP_L("audio option on the ROM screen.")
    HELP_L("Unknown mapper: tag the name, e.g.")
    HELP_L("Game.PLA-32.ROM (see the manual).")
    HELP_L("Odd options: delete NAME.PVC.")
    "WiFi offline: set it again with F4.";

static const help_page_t help_pages_40[HELP_PAGE_COUNT] = {
    HELP_PAGE(help40_browse), HELP_PAGE(help40_files), HELP_PAGE(help40_rom),
    HELP_PAGE(help40_mp3), HELP_PAGE(help40_hunter), HELP_PAGE(help40_about),
};

#endif
