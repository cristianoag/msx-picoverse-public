// Host check of the menu ROM's minimal printf (msx/src/mini_printf.c): every
// format the menu uses must produce exactly what the C library does.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char printed[512];
static int printed_len;

int mini_putchar(int c)
{
    printed[printed_len++] = (char)c;
    return c;
}

#define putchar mini_putchar
#define printf mini_printf
#define sprintf mini_sprintf
#include "../msx/src/mini_printf.c"
#undef putchar
#undef printf
#undef sprintf

static int failures;

#define CHECK(...)                                                               \
    do {                                                                         \
        char got[512];                                                           \
        char want[512];                                                          \
        int got_count = mini_sprintf(got, __VA_ARGS__);                          \
        int want_count = snprintf(want, sizeof(want), __VA_ARGS__);              \
        printed_len = 0;                                                         \
        int printed_count = mini_printf(__VA_ARGS__);                            \
        printed[printed_len] = '\0';                                             \
        if (strcmp(got, want) != 0 || got_count != want_count ||                 \
            strcmp(printed, want) != 0 || printed_count != want_count) {         \
            printf("FAIL %s\n  got  [%s] (%d)\n  want [%s] (%d)\n", #__VA_ARGS__, \
                   got, got_count, want, want_count);                            \
            failures++;                                                          \
        }                                                                        \
    } while (0)

int main(void)
{
    const char *long_name = "A very long ROM name that does not fit in the narrow column of the 40-column list";
    const char *rows[] = {"", "Y", "Game", "Exactly twenty-one ch", "Exactly twenty-one chars", long_name};

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        CHECK(" %-61.61s %6s %-2s %-5s", rows[i], "512KB", "SD", "ASC16");
        CHECK(" %-21.21s %6s %-2s %-5s", rows[i], "16MB", "FL", "");
        CHECK("   Name: %-71.71s", rows[i]);
        CHECK("   Name: %-29.29s", rows[i]);
        CHECK("   %s: %-71.71s", "ROM", rows[i]);
        CHECK("   %s: %-29.29s", "DSK", rows[i]);
        CHECK("%s", rows[i]);
        CHECK("%s%s", "Name: ", rows[i]);
        CHECK("Chip ID: %.16s", rows[i]);
        CHECK("   Size: %-8.8s", rows[i]);
        CHECK("   Type: %s", rows[i]);
    }

    unsigned long longs[] = {0, 1, 9, 10, 99, 100, 4095, 65535, 65536, 1000000UL, 999999999UL, 1000000000UL, 4294967295UL};
    for (size_t i = 0; i < sizeof(longs) / sizeof(longs[0]); i++) {
        CHECK("   Size: %lu KB", longs[i]);
        CHECK("%luKB", longs[i]);
        CHECK("%luMB", longs[i]);
        CHECK("%lu", longs[i]);
    }

    unsigned ints[] = {0, 1, 9, 10, 59, 99, 100, 1023, 16384, 65535};
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        CHECK("%uKB", ints[i]);
        CHECK("%uMB", ints[i]);
        CHECK("    Volume: %u%%", ints[i]);
        CHECK("%s: %u%%", "Flash", ints[i]);
        CHECK("Available: %u.%u MB", ints[i], ints[(i + 3) % 10]);
        CHECK("   Play: %02u:%02u   %-8.8s", ints[i], ints[(i + 5) % 10], "Playing");
    }

    int numbers[] = {0, 1, 7, 12, 99, 100, 32767, -1, -12, -32768};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        CHECK("Page %02d/%02d", numbers[i], numbers[(i + 1) % 10]);
        CHECK("%d|%5d|%-5d|%05d", numbers[i], numbers[i], numbers[i], numbers[i]);
    }

    CHECK("MSX PICOVERSE 2350%44s[EXPLORER %s]", "", "v2.61");
    CHECK("MSX PICOVERSE 2350%4s[EXPLORER %s]", "", "v2.61");
    CHECK("Listing:%s", " ROM");
    CHECK("%%");
    CHECK("100%%|%5s|%-5s|", "ab", "cd");
    CHECK("no conversions");
    CHECK("");

    if (failures) {
        printf("mini printf: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: mini printf matches the C library for every menu format\n");
    return 0;
}
