// Host tests for the microSD side of R (rename) in pico/explorer/explorer.c:
// cleaning the requested name, keeping the mapper tag and extension of the
// old file, and the .PVC options path that follows the file.
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rename-production.h"

static void expect_clean(const char *input, const char *expected) {
    char name[ROM_NAME_MAX];
    memset(name, 0, sizeof(name));
    strncpy(name, input, sizeof(name) - 1);
    bool ok = rename_clean_name(name);
    if (expected) {
        assert(ok && strcmp(name, expected) == 0);
    } else {
        assert(!ok);
    }
}

static void expect_path(const char *old_path, const char *name, const char *expected) {
    char out[SD_PATH_MAX];
    if (expected) {
        assert(build_renamed_path(old_path, name, out, sizeof(out)));
        assert(strcmp(out, expected) == 0);
    } else {
        assert(!build_renamed_path(old_path, name, out, sizeof(out)));
    }
}

static void expect_pvc(const char *rom_path, bool is_dsk, const char *expected) {
    char out[SD_PATH_MAX];
    assert(sd_pvc_path_from_rom(rom_path, is_dsk, out, sizeof(out)));
    assert(strcmp(out, expected) == 0);
    // In place, as sd_rename_entry() uses it.
    strcpy(out, rom_path);
    assert(sd_pvc_path_from_rom(out, is_dsk, out, sizeof(out)));
    assert(strcmp(out, expected) == 0);
}

static void expect_display(const char *filename, const char *expected) {
    char out[ROM_NAME_MAX + 1];
    assert(display_name_length(filename) == strlen(expected));
    build_display_name(filename, out, sizeof(out));
    assert(strcmp(out, expected) == 0);
}

int main(void) {
    expect_clean("Knight Mare", "Knight Mare");
    expect_clean("  Padded Name  ", "Padded Name");
    expect_clean("Trailing...", "Trailing");
    expect_clean("Space (1987) [!] v1.1", "Space (1987) [!] v1.1");
    expect_clean("", NULL);
    expect_clean("    ", NULL);
    expect_clean("..", NULL);
    for (const char *c = "\\/:*?\"<>|"; *c; c++) {
        char bad[8] = {'A', *c, 'B', '\0'};
        expect_clean(bad, NULL);
    }
    expect_clean("Tab\there", NULL);
    expect_clean("Del\x7F", NULL);
    char long_name[ROM_NAME_MAX];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    assert(rename_clean_name(long_name) && strlen(long_name) == RENAME_NAME_MAX);

    // The menu name stops before the mapper tag; R keeps tag and extension.
    expect_display("Knight Mare.PLA-32.ROM", "Knight Mare");
    expect_display("Game.ROM", "Game");
    expect_display("Disk Game.dsk", "Disk Game");
    expect_display("Foo.Bar.ROM", "Foo.Bar");
    expect_display("Saver.asc16x-fr.rom", "Saver");
    expect_display("Odd.SYSTEM.ROM", "Odd.SYSTEM");

    expect_path("/Knight Mare.PLA-32.ROM", "Nightmare", "/Nightmare.PLA-32.ROM");
    expect_path("/GAMES/RPG/Game.rom", "Better Game", "/GAMES/RPG/Better Game.rom");
    expect_path("/Disks/Disk Game.DSK", "Disk Game (Side B)", "/Disks/Disk Game (Side B).DSK");
    expect_path("/Saver.ASC16X-FR.ROM", "Saver 2", "/Saver 2.ASC16X-FR.ROM");
    expect_path("/Foo.Bar.ROM", "Baz", "/Baz.ROM");
    char deep[SD_PATH_MAX];
    memset(deep, 'd', sizeof(deep));
    deep[0] = '/';
    deep[SD_PATH_MAX - 12] = '/';
    strcpy(deep + SD_PATH_MAX - 11, "Game.ROM");
    expect_path(deep, "Game", deep);
    expect_path(deep, "A Much Longer Name", NULL);

    expect_pvc("/GAMES/Game.ROM", false, "/GAMES/Game.PVC");
    expect_pvc("/Knight Mare.PLA-32.ROM", false, "/Knight Mare.PLA-32.PVC");
    expect_pvc("/Disks/Game.DSK", true, "/Disks/Game.DSK.PVC");
    expect_pvc("/A.B/Game", false, "/A.B/Game.PVC");

    puts("PASS: rename name cleaning, tag/extension kept on microSD, .PVC paths follow");
    return 0;
}
