// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// screen_rom.c - MSX Explorer ROM detail screen (options, mapper, launch)
//
// This work is licensed  under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "menu.h"
#include "menu_state.h"
#include "menu_ui.h"
#include "menu_input.h"
#include "screen_rom.h"

static void send_detect_mapper(unsigned int index);
static unsigned char send_set_mapper(unsigned int index, unsigned char mapper);
static unsigned char send_load_options(unsigned int index, unsigned char *audio_profile, unsigned char *psg_enabled, unsigned char *mapper, unsigned char *sd_partition, unsigned char *audio_volume, unsigned char *vdp_freq);
static void send_save_options(unsigned int index, unsigned char audio_profile, unsigned char psg_enabled, unsigned char mapper, unsigned char sd_partition, unsigned char audio_volume, unsigned char vdp_freq);
static unsigned char read_mapper_value(void);
static void build_cpu_mode_list(void);
static void select_cpu_mode(unsigned char cpu_mode);
static void step_cpu_mode(int dir);
static void compute_option_selections(unsigned char allow_wifi_support);
static void render_rom_screen(const ROMRecord *record);
static void render_rom_prefixed_line(unsigned char row, const char *prefix, const char *text, int selected);
static void render_rom_mapper_line(const char *mapper_text, int selected);
static void render_rom_audio_line(const char *audio_text, int selected);
static void render_rom_volume_line(unsigned char audio_volume, int selected);
static void render_rom_psg_line(unsigned char psg_enabled, int selected);
static void render_rom_freq_line(unsigned char row, unsigned char vdp_freq, int selected);
static void render_rom_cpu_line(unsigned char row, unsigned char cpu_mode, int selected);
static void render_rom_partition_line(unsigned char row, unsigned char sd_partition, int selected);
static void render_rom_wifi_line(unsigned char row, unsigned char wifi_enabled, int selected);
static void render_rom_action_line(unsigned char row, int selected);
static void render_rom_options_block(void);
static void render_rom_footer_line(void);
static void show_mp3_screen(unsigned int index);
static void render_mp3_screen(const ROMRecord *record);
static void render_mp3_counter_line(void);
static void render_mp3_footer_line(void);
static void send_mp3_select(unsigned int index);
static unsigned int read_mp3_elapsed(void);
static unsigned int read_mp3_selected_index(void);
static ROMRecord *load_mp3_detail_record(unsigned int index);
static void build_mapper_text(const ROMRecord *record, int waiting_mapper, char *out, size_t out_size);
static void build_audio_text(const ROMRecord *record, unsigned char audio_profile, char *out, size_t out_size);
static unsigned char sanitize_audio_profile(const ROMRecord *record, unsigned char audio_profile);
static unsigned char next_audio_profile(const ROMRecord *record, unsigned char audio_profile, int dir);
static void apply_detected_mapper(ROMRecord *record);
static unsigned char current_wavegame_rom = 0;

#define MP3_COUNTER_POLL_JIFFIES 5
#define MP3_COUNTER_FORCE_JIFFIES 50

static unsigned char sd_partition_count = 0;
static unsigned char sd_partition_mask = 0;
static unsigned char rom_sd_partition = 0;
static unsigned char rom_audio_volume = AUDIO_VOLUME_DEFAULT;
static unsigned char rom_vdp_freq = VDP_FREQ_DEFAULT;
static unsigned char rom_allow_sd_partition = 0;
static unsigned char rom_allow_freq = 0;
/* Per-ROM CPU speed option. cpu_modes[] lists the values this machine can
   actually deliver (always CPU_MODE_DEFAULT, plus CPU_MODE_TURBO when the
   Panasonic switched-I/O turbo device answers and CPU_MODE_R800 on a turbo R),
   so the option row is hidden entirely on machines that support neither. */
static unsigned char rom_cpu_mode = CPU_MODE_DEFAULT;
static unsigned char rom_allow_cpu = 0;
static unsigned char cpu_modes[3];
static unsigned char cpu_mode_count = 1;
static unsigned char cpu_mode_index = 0;
/* Row/selection indices for the option block, computed once per screen so the
   renderer and the key handler cannot drift apart. */
static int sel_freq, sel_cpu, sel_part, sel_wifi, sel_action;
/* Live state of the ROM detail screen. Held here rather than passed on every
   render call because render_rom_options_block() is invoked from a dozen key
   handlers and the argument marshalling alone cost hundreds of ROM bytes. */
static ROMRecord *cur_record;
static unsigned char cur_waiting_mapper;
static unsigned char cur_audio_profile;
static unsigned char cur_psg_enabled;
static unsigned char cur_wifi_enabled;
static unsigned char rom_allow_wifi;
static int cur_selection;

static void write_index_query(unsigned int index) {
    Poke(CTRL_QUERY_BASE + 0, (unsigned char)(index & 0xFFu));
    Poke(CTRL_QUERY_BASE + 1, (unsigned char)((index >> 8) & 0xFFu));
}

static void clear_query_tail(unsigned char start) {
    for (unsigned char i = start; i < CTRL_QUERY_SIZE; i++) {
        Poke(CTRL_QUERY_BASE + i, 0);
    }
}

static unsigned char record_is_system_rom(const ROMRecord *record) {
    unsigned char mapper_code = record_mapper_code(record->Mapper);
    return mapper_code == 9 || (mapper_code >= 10 && mapper_code <= 11) || (mapper_code >= 15 && mapper_code <= 21);
}

static unsigned char record_is_wifi_capable_system_rom(const ROMRecord *record) {
    unsigned char mapper_code = record_mapper_code(record->Mapper);
    return (mapper_code >= 10 && mapper_code <= 11) || (mapper_code >= 15 && mapper_code <= 16);
}

static unsigned char record_is_sunrise_system_rom(const ROMRecord *record) {
    unsigned char mapper_code = record_mapper_code(record->Mapper);
    return (mapper_code >= 10 && mapper_code <= 11) || (mapper_code >= 15 && mapper_code <= 18);
}

static unsigned char record_is_sunrise_sd_system_rom(const ROMRecord *record) {
    unsigned char mapper_code = record_mapper_code(record->Mapper);
    return mapper_code >= 15 && mapper_code <= 19 && mapper_code != 18;
}

static unsigned char record_is_sunrise_mapper_system_rom(const ROMRecord *record) {
    unsigned char mapper_code = record_mapper_code(record->Mapper);
    return mapper_code == 11 || (mapper_code >= 16 && mapper_code <= 21);
}

static unsigned char record_supports_scc_audio(const ROMRecord *record) {
    unsigned char mapper_code = record_mapper_code(record->Mapper);
    return mapper_code == 3 || mapper_code == 14;
}

static unsigned char record_supports_external_scc_audio(const ROMRecord *record) {
    return !record_is_folder(record) && (!record_is_system_rom(record) || record_is_sunrise_system_rom(record));
}

static unsigned char record_supports_dual_psg(const ROMRecord *record) {
    return record_supports_external_scc_audio(record) && !record_supports_scc_audio(record);
}

static unsigned char record_supports_msx_music(const ROMRecord *record) {
    /* Base support shared with YM2151/SFG. YM2413/FM-PAC (MSX-MUSIC) additionally
       excludes the Sunrise 1MB-mapper options in audio_profile_is_supported(). */
    return record_supports_external_scc_audio(record);
}

static void send_detect_mapper(unsigned int index) {
    write_index_query(index);
    clear_query_tail(2);
    Poke(CTRL_CMD, CMD_DETECT_MAPPER);
}

static unsigned char send_set_mapper(unsigned int index, unsigned char mapper) {
    write_index_query(index);
    Poke(CTRL_QUERY_BASE + 2, mapper);
    clear_query_tail(3);
    return run_ctrl_cmd(CMD_SET_MAPPER);
}

static unsigned char send_load_options(unsigned int index, unsigned char *audio_profile, unsigned char *psg_enabled, unsigned char *mapper, unsigned char *sd_partition, unsigned char *audio_volume, unsigned char *vdp_freq) {
    write_index_query(index);
    clear_query_tail(2);
    unsigned char options_ack = run_ctrl_cmd(CMD_LOAD_OPTIONS);
    current_wavegame_rom = Peek(CTRL_WAVEGAME_ROM) ? 1 : 0;
    if (!options_ack) {
        return 0;
    }
    *audio_profile = Peek(CTRL_AUDIO);
    *psg_enabled = Peek(CTRL_PSG_EMULATION) ? 1 : 0;
    *mapper = Peek(CTRL_MAPPER);
    *sd_partition = Peek(CTRL_SD_PARTITION);
    *audio_volume = Peek(CTRL_AUDIO_VOLUME);
    *vdp_freq = Peek(CTRL_VDP_FREQ);
    if (*vdp_freq > VDP_FREQ_50HZ) {
        *vdp_freq = VDP_FREQ_DEFAULT;
    }
    rom_cpu_mode = Peek(CTRL_CPU_MODE);
    select_cpu_mode(rom_cpu_mode);
    sd_partition_count = Peek(CTRL_SD_PARTITION_INFO_BASE);
    sd_partition_mask = Peek(CTRL_SD_PARTITION_INFO_BASE + 1);
    return 1;
}

static void send_save_options(unsigned int index, unsigned char audio_profile, unsigned char psg_enabled, unsigned char mapper, unsigned char sd_partition, unsigned char audio_volume, unsigned char vdp_freq) {
    write_index_query(index);
    Poke(CTRL_QUERY_BASE + 2, audio_profile);
    Poke(CTRL_QUERY_BASE + 3, psg_enabled ? 1 : 0);
    Poke(CTRL_QUERY_BASE + 4, mapper);
    Poke(CTRL_QUERY_BASE + 5, sd_partition);
    Poke(CTRL_QUERY_BASE + 6, audio_volume);
    Poke(CTRL_QUERY_BASE + 7, vdp_freq);
    Poke(CTRL_QUERY_BASE + 8, rom_allow_cpu ? rom_cpu_mode : CPU_MODE_DEFAULT);
    clear_query_tail(9);
    Poke(CTRL_CMD, CMD_SAVE_OPTIONS);
    wait_ctrl_cmd();
}

static unsigned char read_mapper_value(void) {
    return *((unsigned char *)CTRL_MAPPER);
}

/* Builds the list of CPU speeds this machine can deliver, entirely in assembly
   to keep the menu ROM small. The Panasonic switched-I/O turbo device answers
   on port #40 with the complement of the selected manufacturer id (8), and port
   #41 bit 2 reports whether the 5.37MHz turbo is fitted (0 = available). The
   previously selected id is saved and restored so the probe is harmless on
   machines without expanded I/O. MSXVER (main ROM #002D) is 3 on a turbo R,
   which is the only family with CHGCPU/R800. */
static void build_cpu_mode_list(void) {
    __asm
        ld   hl,#_cpu_modes
        ld   (hl),#0
        inc  hl
        ld   c,#1
        in   a,(#0x40)
        cpl
        push af
        ld   a,#8
        out  (#0x40),a
        in   a,(#0x40)
        cpl
        cp   #8
        jr   nz,cpu_no_pana
        in   a,(#0x41)
        bit  2,a
        jr   nz,cpu_no_pana
        ld   (hl),#1
        inc  hl
        inc  c
    cpu_no_pana:
        pop  af
        cpl
        out  (#0x40),a
        ld   a,(#0x002D)
        cp   #3
        jr   nz,cpu_no_r800
        ld   (hl),#2
        inc  c
    cpu_no_r800:
        ld   a,c
        ld   (_cpu_mode_count),a
    __endasm;
}

static void select_cpu_mode(unsigned char cpu_mode) {
    unsigned char i;
    cpu_mode_index = 0;
    for (i = 0; i < cpu_mode_count; i++) {
        if (cpu_modes[i] == cpu_mode) {
            cpu_mode_index = i;
            break;
        }
    }
    rom_cpu_mode = cpu_modes[cpu_mode_index];
}

static void step_cpu_mode(int dir) {
    if (dir > 0) {
        cpu_mode_index++;
        if (cpu_mode_index >= cpu_mode_count) {
            cpu_mode_index = 0;
        }
    } else {
        if (cpu_mode_index == 0) {
            cpu_mode_index = cpu_mode_count;
        }
        cpu_mode_index--;
    }
    rom_cpu_mode = cpu_modes[cpu_mode_index];
}

static void compute_option_selections(unsigned char allow_wifi_support) {
    int n = 4;
    sel_freq = rom_allow_freq ? n++ : -1;
    sel_cpu = rom_allow_cpu ? n++ : -1;
    sel_part = rom_allow_sd_partition ? n++ : -1;
    sel_wifi = allow_wifi_support ? n++ : -1;
    sel_action = n;
}

static void apply_detected_mapper(ROMRecord *record) {
    unsigned char mapper = read_mapper_value();
    if (mapper != 0) {
        record->Mapper = (record->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG)) | mapper;
    } else {
        record->Mapper = (record->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG));
    }
}

void quick_run_rom(unsigned int index) {
    ROMRecord *record = &records[index % FILES_PER_PAGE];
    write_index_query(index);
    (void)run_ctrl_cmd(CMD_PREPARE_QUICK_RUN);
    apply_detected_mapper(record);
    loadGame((int)index);
}

static void render_rom_screen(const ROMRecord *record) {
    menu_ui_render_detail_frame();

    {
        char name[MAX_FILE_NAME_LENGTH + 1];
        const char *source = (record->Mapper & SOURCE_SD_FLAG) ? "SD" : "FL";
        unsigned long size_kb = record->Size / 1024u;

        trim_name_to_buffer(record->Name, name, use_80_columns ? 71 : 29);

        Locate(0, 3);
        if (use_80_columns) {
            printf("    ROM: %-71.71s", name);
        } else {
            printf("    ROM: %-29.29s", name);
        }

        Locate(0, 4);
        printf("   Size: %lu KB", size_kb);
        Locate(0, 5);
        printf(" Source: %s", source);
        Locate(0, 6);
        printf(" ");
    }
}

void show_rom_screen(unsigned int index) {
    ROMRecord *record = &records[index % FILES_PER_PAGE];
    unsigned char saved_mapper = 0;
    unsigned char sd_partition = 0;
    unsigned char options_loaded = 0;
    unsigned char allow_mapper_override = !record_is_system_rom(record);
    unsigned char allow_sd_partition = record_is_sunrise_sd_system_rom(record);
    unsigned char allow_psg = !record_is_sunrise_mapper_system_rom(record);
    /* VDP R9 (50/60Hz) exists only on the V9938/V9958 (MSX2+). The MSX version
       byte at main-ROM 0x002D is 0 on MSX1, so the frequency option is offered
       only when it is non-zero. */
    unsigned char allow_freq = !record_is_system_rom(record) && (Peek(0x002D) != 0);
    int volume_selection = 2;
    int psg_selection = 3;

    cur_record = record;
    cur_waiting_mapper = 0;
    cur_audio_profile = AUDIO_PROFILE_NONE;
    cur_psg_enabled = 1;
    cur_wifi_enabled = 0;
    rom_allow_wifi = record_is_wifi_capable_system_rom(record);

    build_cpu_mode_list();
    rom_allow_sd_partition = allow_sd_partition;
    rom_allow_freq = allow_freq;
    /* The CPU speed patch is injected into the launched ROM's cartridge INIT,
       so it only applies to regular game ROMs; the system ROMs manage their own
       boot. The row is also hidden when this machine offers no alternative. */
    rom_allow_cpu = !record_is_system_rom(record) && (cpu_mode_count > 1);
    compute_option_selections(rom_allow_wifi);
    cur_selection = sel_action;

    rom_audio_volume = AUDIO_VOLUME_DEFAULT;
    rom_vdp_freq = VDP_FREQ_DEFAULT;
    rom_cpu_mode = CPU_MODE_DEFAULT;
    cpu_mode_index = 0;

    if (record_is_mp3(record)) {
        show_mp3_screen(index);
        return;
    }

    render_rom_screen(record);

    if ((record->Mapper & SOURCE_SD_FLAG) && !record_is_folder(record)) {
        unsigned char mapper_code = record_mapper_code(record->Mapper);
        if (mapper_code == 0) {
            wait_ctrl_cmd();
            send_detect_mapper(index);
            cur_waiting_mapper = 1;
        } else if (record_supports_scc_audio(record)) {
            cur_audio_profile = AUDIO_PROFILE_SCC;
        }
    } else {
        if (record_supports_scc_audio(record)) {
            cur_audio_profile = AUDIO_PROFILE_SCC;
        }
    }

    if (!cur_waiting_mapper) {
        options_loaded = send_load_options(index, &cur_audio_profile, &cur_psg_enabled, &saved_mapper, &sd_partition, &rom_audio_volume, &rom_vdp_freq);
        if (options_loaded && saved_mapper != 0 && allow_mapper_override) {
                record->Mapper = (record->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG)) | saved_mapper;
        }
    }
    cur_audio_profile = sanitize_audio_profile(record, cur_audio_profile);
    if (!allow_psg) {
        cur_psg_enabled = 0; // PSG Mirror is disabled on Nextor + 1MB mapper (unstable)
    }

    rom_sd_partition = sd_partition;
    render_rom_options_block();

    while (1) {
        if (bios_chsns()) {
            char key = (char)bios_chget();
            if (key == MENU_KEY_F4_CONFIG) {
                launch_wifi_config();
                return;
            }
            if (key == 'h' || key == 'H') {
                helpMenu();
                render_rom_screen(record);
                render_rom_options_block();
            }
            if (key == 27) {
                break;
            }
            if (key == 13 || key == 32) {
                if (!cur_waiting_mapper && cur_selection == sel_action) {
                    cur_audio_profile = sanitize_audio_profile(record, cur_audio_profile);
                    Poke(CTRL_AUDIO, cur_audio_profile);
                    Poke(CTRL_PSG_EMULATION, cur_psg_enabled);
                    Poke(CTRL_WIFI_SUPPORT, rom_allow_wifi ? cur_wifi_enabled : 0);
                    Poke(CTRL_SD_PARTITION, allow_sd_partition ? sd_partition : 0);
                    Poke(CTRL_AUDIO_VOLUME, rom_audio_volume);
                    send_save_options(index, cur_audio_profile, cur_psg_enabled, record_mapper_code(record->Mapper), allow_sd_partition ? sd_partition : 0, rom_audio_volume, allow_freq ? rom_vdp_freq : VDP_FREQ_DEFAULT);
                    loadGame((int)index);
                    return;
                }
            }
            if (key == 30 || key == 31) {
                int delta = (key == 30) ? -1 : 1;
                int next = cur_selection + delta;
                if (next < 0) {
                    next = 0;
                } else if (next > sel_action) {
                    next = sel_action;
                }
                if (cur_selection != next) {
                    cur_selection = next;
                    render_rom_options_block();
                }
            }
            if ((key == 28 || key == 29) && cur_selection == 0 && !cur_waiting_mapper && allow_mapper_override) {
                static const unsigned char mapper_cycle[] = {1,2,3,4,5,6,7,8,9,12,13,14};
                const unsigned int mapper_count = (unsigned int)(sizeof(mapper_cycle) / sizeof(mapper_cycle[0]));
                unsigned char mapper_code = record_mapper_code(record->Mapper);
                int dir = (key == 28) ? 1 : -1;
                int found = -1;
                for (unsigned int i = 0; i < mapper_count; i++) {
                    if (mapper_cycle[i] == mapper_code) {
                        found = (int)i;
                        break;
                    }
                }
                int next_index = 0;
                if (found < 0) {
                    next_index = (dir > 0) ? 0 : (int)(mapper_count - 1);
                } else {
                    next_index = found + dir;
                    if (next_index < 0) {
                        next_index = (int)(mapper_count - 1);
                    } else if ((unsigned int)next_index >= mapper_count) {
                        next_index = 0;
                    }
                }
                unsigned char next_mapper = mapper_cycle[next_index];
                if (send_set_mapper(index, next_mapper)) {
                    record->Mapper = (record->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG)) | next_mapper;
                    cur_audio_profile = sanitize_audio_profile(record, cur_audio_profile);
                    render_rom_options_block();
                }
            }
            if ((key == 28 || key == 29) && cur_selection == 1) {
                int dir = (key == 28) ? 1 : -1;
                cur_audio_profile = next_audio_profile(record, cur_audio_profile, dir);
                render_rom_options_block();
            }
            if ((key == 28 || key == 29) && cur_selection == volume_selection) {
                if (key == 28) {
                    if (rom_audio_volume < AUDIO_VOLUME_MAX) rom_audio_volume += AUDIO_VOLUME_STEP;
                } else {
                    if (rom_audio_volume >= AUDIO_VOLUME_STEP) rom_audio_volume -= AUDIO_VOLUME_STEP;
                }
                render_rom_options_block();
            }
            if ((key == 28 || key == 29) && cur_selection == psg_selection && allow_psg) {
                cur_psg_enabled = cur_psg_enabled ? 0 : 1;
                render_rom_options_block();
            }
            if ((key == 28 || key == 29) && rom_allow_freq && cur_selection == sel_freq) {
                if (key == 28) {
                    rom_vdp_freq = (rom_vdp_freq >= VDP_FREQ_50HZ) ? VDP_FREQ_DEFAULT : (unsigned char)(rom_vdp_freq + 1);
                } else {
                    rom_vdp_freq = (rom_vdp_freq == VDP_FREQ_DEFAULT) ? VDP_FREQ_50HZ : (unsigned char)(rom_vdp_freq - 1);
                }
                render_rom_options_block();
            }
            if ((key == 28 || key == 29) && rom_allow_cpu && cur_selection == sel_cpu) {
                step_cpu_mode((key == 28) ? 1 : -1);
                render_rom_options_block();
            }
            if ((key == 28 || key == 29) && rom_allow_sd_partition && cur_selection == sel_part && sd_partition_count) {
                int dir = (key == 28) ? 1 : -1;
                unsigned char next_part = sd_partition;
                for (unsigned char i = 0; i < 4; i++) {
                    next_part = (unsigned char)(next_part + dir);
                    if (next_part < 1) {
                        next_part = 4;
                    } else if (next_part > 4) {
                        next_part = 1;
                    }
                    if (sd_partition_mask & (1 << (next_part - 1))) {
                        break;
                    }
                }
                sd_partition = next_part;
                rom_sd_partition = sd_partition;
                Poke(CTRL_SD_PARTITION, sd_partition);
                render_rom_options_block();
            }
            if ((key == 28 || key == 29) && rom_allow_wifi && cur_selection == sel_wifi) {
                cur_wifi_enabled = cur_wifi_enabled ? 0 : 1;
                render_rom_options_block();
            }
            if (key == 'C' || key == 'c') {
                if (menu_ui_try_toggle_columns()) {
                    render_rom_screen(record);
                    render_rom_options_block();
                }
            }
        }

        if (cur_waiting_mapper && Peek(CTRL_CMD) == 0) {
            apply_detected_mapper(record);
            if (cur_audio_profile == AUDIO_PROFILE_NONE && record_supports_scc_audio(record)) {
                cur_audio_profile = AUDIO_PROFILE_SCC;
            } else {
                cur_audio_profile = sanitize_audio_profile(record, cur_audio_profile);
            }
            if (!options_loaded) {
                options_loaded = send_load_options(index, &cur_audio_profile, &cur_psg_enabled, &saved_mapper, &sd_partition, &rom_audio_volume, &rom_vdp_freq);
                if (options_loaded && saved_mapper != 0 && allow_mapper_override) {
                    record->Mapper = (record->Mapper & (SOURCE_SD_FLAG | FOLDER_FLAG)) | saved_mapper;
                }
            }
            cur_audio_profile = sanitize_audio_profile(record, cur_audio_profile);
            cur_waiting_mapper = 0;
            rom_sd_partition = sd_partition;
            render_rom_options_block();
        }

        delay_ms(10);
    }

    frame_rendered = 0;
    displayMenu();
}

static void render_rom_prefixed_line(unsigned char row, const char *prefix, const char *text, int selected) {
    char line[80];
    size_t prefix_len = strlen(prefix);
    size_t out_len = 0;
    if (prefix_len >= sizeof(line)) {
        prefix_len = sizeof(line) - 1;
    }
    memcpy(line, prefix, prefix_len);
    out_len = prefix_len;
    line[out_len] = '\0';

    if (text && out_len < (sizeof(line) - 1)) {
        size_t remaining = sizeof(line) - 1 - out_len;
        size_t text_len = strlen(text);
        if (text_len > remaining) {
            text_len = remaining;
        }
        memcpy(line + out_len, text, text_len);
        out_len += text_len;
        line[out_len] = '\0';
    }

    menu_ui_render_selectable_line(row, line, selected);
}

static void render_rom_mapper_line(const char *mapper_text, int selected) {
    render_rom_prefixed_line(7, "    Mapper: ", mapper_text, selected);
}

static void render_rom_audio_line(const char *audio_text, int selected) {
    render_rom_prefixed_line(8, "     Audio: ", audio_text, selected);
}

static void render_rom_volume_line(unsigned char audio_volume, int selected) {
    char line[80];
    sprintf(line, "    Volume: %u%%", audio_volume);
    menu_ui_render_selectable_line(9, line, selected);
}

static void render_rom_psg_line(unsigned char psg_enabled, int selected) {
    render_rom_prefixed_line(10, "PSG Mirror: ", psg_enabled ? "Yes" : "No", selected);
}

static void render_rom_freq_line(unsigned char row, unsigned char vdp_freq, int selected) {
    const char *text = "Default";
    if (vdp_freq == VDP_FREQ_60HZ) {
        text = "60Hz";
    } else if (vdp_freq == VDP_FREQ_50HZ) {
        text = "50Hz";
    }
    render_rom_prefixed_line(row, " Frequency: ", text, selected);
}

static void render_rom_cpu_line(unsigned char row, unsigned char cpu_mode, int selected) {
    const char *text = "Default";
    if (cpu_mode == CPU_MODE_TURBO) {
        text = "Turbo";
    } else if (cpu_mode == CPU_MODE_R800) {
        text = "R800";
    }
    render_rom_prefixed_line(row, "       CPU: ", text, selected);
}

static void render_rom_partition_line(unsigned char row, unsigned char sd_partition, int selected) {
    char label[31];
    unsigned char i;
    (void)sd_partition;
    for (i = 0; i < sizeof(label) - 1; i++) {
        label[i] = (char)Peek(CTRL_SD_PARTITION_INFO_BASE + 2 + i);
        if (!label[i]) {
            break;
        }
    }
    label[sizeof(label) - 1] = '\0';
    render_rom_prefixed_line(row, "   SD Part: ", label, selected);
}

static void render_rom_wifi_line(unsigned char row, unsigned char wifi_enabled, int selected) {
    render_rom_prefixed_line(row, "      Wifi: ", wifi_enabled ? "Yes" : "No", selected);
}

static void render_rom_action_line(unsigned char row, int selected) {
    menu_ui_render_selectable_line(row, "    Action: Run", selected);
}

static void render_rom_options_block(void) {
    char mapper_text[48];
    char audio_text[32];
    int volume_selection = 2;
    int psg_selection = 3;
    unsigned char row = 11;

    build_mapper_text(cur_record, cur_waiting_mapper, mapper_text, sizeof(mapper_text));
    build_audio_text(cur_record, cur_audio_profile, audio_text, sizeof(audio_text));
    render_rom_mapper_line(mapper_text, cur_selection == 0);
    render_rom_audio_line(audio_text, cur_selection == 1);
    render_rom_volume_line(rom_audio_volume, cur_selection == volume_selection);
    render_rom_psg_line(cur_psg_enabled, cur_selection == psg_selection);
    if (rom_allow_freq) {
        render_rom_freq_line(row++, rom_vdp_freq, cur_selection == sel_freq);
    }
    if (rom_allow_cpu) {
        render_rom_cpu_line(row++, rom_cpu_mode, cur_selection == sel_cpu);
    }
    if (rom_allow_sd_partition) {
        render_rom_partition_line(row++, rom_sd_partition, cur_selection == sel_part);
    }
    if (rom_allow_wifi) {
        render_rom_wifi_line(row++, cur_wifi_enabled, cur_selection == sel_wifi);
    }
    render_rom_action_line(row, cur_selection == sel_action);
    render_rom_footer_line();
}

static void render_rom_footer_line(void) {
    menu_ui_clear_rows(22, 24);
    Locate(0, 22);
    printf(menu_ui_status_text("[ESC-BACK] [L/R-CHANGE]", "[ESC - BACK] [LEFT/RIGHT - CHANGE]"));
}
static void send_mp3_select(unsigned int index) {
    Poke(MP3_CTRL_INDEX_L, (unsigned char)(index & 0xFFu));
    Poke(MP3_CTRL_INDEX_H, (unsigned char)((index >> 8) & 0xFFu));
    Poke(MP3_CTRL_CMD, MP3_CMD_SELECT);
}

static unsigned int read_mp3_elapsed(void) {
    return (unsigned int)Peek(MP3_CTRL_ELAPSED_L) |
           ((unsigned int)Peek(MP3_CTRL_ELAPSED_H) << 8);
}

static unsigned int read_mp3_selected_index(void) {
    return (unsigned int)Peek(MP3_CTRL_INDEX_L) |
           ((unsigned int)Peek(MP3_CTRL_INDEX_H) << 8);
}

static ROMRecord *load_mp3_detail_record(unsigned int index) {
    currentIndex = (int)index;
    currentPage = (int)(index / FILES_PER_PAGE) + 1;
    if (paging_enabled) {
        load_page_records((unsigned int)(currentPage - 1));
    }
    return &records[index % FILES_PER_PAGE];
}

static void render_mp3_screen(const ROMRecord *record) {
    char name[MAX_FILE_NAME_LENGTH + 1];
    unsigned long size_kb = record->Size / 1024u;
    const char *type = ((record->Mapper & AUDIO_TYPE_MASK) == AUDIO_TYPE_WAV) ? "WAV" : "MP3";

    menu_ui_render_detail_frame();

    trim_name_to_buffer(record->Name, name, use_80_columns ? 71 : 29);

    Locate(0, 3);
    if (use_80_columns) {
        printf("    %s: %-71.71s", type, name);
    } else {
        printf("    %s: %-29.29s", type, name);
    }

    Locate(0, 4);
    printf("   Type: %s", type);

    Locate(0, 5);
    printf("   Size: %lu KB", size_kb);
    Locate(0, 6);
    printf(" Source: SD");
}

static void render_mp3_counter_line(void) {
    unsigned int elapsed = read_mp3_elapsed();
    unsigned char status = Peek(MP3_CTRL_STATUS);
    const char *state = "Ready";

    if (status & MP3_STATUS_ERROR) {
        state = "Error";
    } else if (status & MP3_STATUS_PLAYING) {
        state = "Playing";
    } else if (status & MP3_STATUS_PAUSED) {
        state = "Paused";
    } else if (status & MP3_STATUS_EOF) {
        state = "Done";
    }

    menu_ui_clear_rows(8, 9);
    Locate(0, 8);
    printf("   Play: %02u:%02u   %-8.8s", elapsed / 60, elapsed % 60, state);

}

static unsigned char mp3_play_stop_command(unsigned char status) {
    return (status & (MP3_STATUS_PLAYING | MP3_STATUS_PAUSED)) ? MP3_CMD_STOP : MP3_CMD_PLAY;
}

static unsigned char mp3_pause_resume_command(unsigned char status) {
    return (status & MP3_STATUS_PAUSED) ? MP3_CMD_RESUME : MP3_CMD_PAUSE;
}

static void render_mp3_actions(int selection, unsigned char status) {
    unsigned char mode = Peek(MP3_CTRL_MODE);
    menu_ui_clear_rows(10, 14);
    menu_ui_render_selectable_line(10,
        (mp3_play_stop_command(status) == MP3_CMD_STOP) ? "Action: Stop" : "Action: Play",
        selection == 0);
    menu_ui_render_selectable_line(11,
        (mp3_pause_resume_command(status) == MP3_CMD_RESUME) ? "Action: Resume" : "Action: Pause",
        selection == 1);
    if (mode == MP3_PLAY_MODE_ALL) {
        menu_ui_render_selectable_line(12, "  Mode: All", selection == 2);
    } else if (mode == MP3_PLAY_MODE_RANDOM) {
        menu_ui_render_selectable_line(12, "  Mode: Random", selection == 2);
    } else {
        menu_ui_render_selectable_line(12, "  Mode: Single", selection == 2);
    }
}

static void render_mp3_footer_line(void) {
    unsigned char mode = Peek(MP3_CTRL_MODE);
    unsigned char multi_song = mode != MP3_PLAY_MODE_SINGLE;
    const char *text;
    menu_ui_clear_rows(22, 23);
    Locate(0, 22);
    if (use_80_columns) {
        text = multi_song ? "[ESC - BACK] [ENTER - ACTION] [N - NEXT SONG] [P - PREVIOUS SONG]" : "[ESC - BACK] [ENTER - ACTION]";
    } else {
        text = multi_song ? "[ESC - BACK] [N - NEXT] [P - PREVIOUS]" : "[ESC - BACK] [ENT - ACTION]";
    }
    printf(text);
}

static void show_mp3_screen(unsigned int index) {
    unsigned int detail_index = index;
    unsigned int last_detail_index = index;
    ROMRecord *record = &records[detail_index % FILES_PER_PAGE];
    volatile unsigned int *jiffyPtr = (volatile unsigned int *)JIFFY;
    unsigned int last_counter_tick = *jiffyPtr;
    unsigned int last_force_tick = last_counter_tick;
    unsigned int last_elapsed = 0xFFFFu;
    unsigned char last_status = 0xFFu;
    int selection = 0;

    send_mp3_select(detail_index);
    render_mp3_screen(record);
    render_mp3_counter_line();
    last_elapsed = read_mp3_elapsed();
    last_status = Peek(MP3_CTRL_STATUS);
    render_mp3_actions(selection, last_status);
    render_mp3_footer_line();

    while (1) {
        if (bios_chsns()) {
            char key = (char)bios_chget();
            if (key == MENU_KEY_F4_CONFIG) {
                launch_wifi_config();
                return;
            }
            if (key == 'h' || key == 'H') {
                helpMenu();
                record = &records[detail_index % FILES_PER_PAGE];
                render_mp3_screen(record);
                render_mp3_counter_line();
                render_mp3_actions(selection, last_status);
                render_mp3_footer_line();
            }
            if (key == 27) {
                Poke(MP3_CTRL_CMD, MP3_CMD_STOP);
                break;
            }
            if (key == 13 || key == 32) {
                unsigned char status = Peek(MP3_CTRL_STATUS);
                unsigned char cmd = 0;
                if (selection == 0) {
                    cmd = mp3_play_stop_command(status);
                    if (cmd == MP3_CMD_PLAY) {
                        save_last_selection(detail_index);
                        send_mp3_select(detail_index);
                    }
                } else if (selection == 1) {
                    cmd = mp3_pause_resume_command(status);
                } else if (selection == 2) {
                    unsigned char mode = Peek(MP3_CTRL_MODE);
                    mode = (mode >= MP3_PLAY_MODE_RANDOM) ? MP3_PLAY_MODE_SINGLE : (unsigned char)(mode + 1u);
                    Poke(MP3_CTRL_MODE, mode);
                    render_mp3_actions(selection, status);
                    render_mp3_footer_line();
                }
                if (cmd) {
                    Poke(MP3_CTRL_CMD, cmd);
                    status = Peek(MP3_CTRL_STATUS);
                    last_elapsed = read_mp3_elapsed();
                    render_mp3_counter_line();
                    render_mp3_actions(selection, status);
                    last_status = status;
                    last_force_tick = *jiffyPtr;
                }
            }
            if (key == 30 || key == 31) {
                int next = selection + ((key == 30) ? -1 : 1);
                if (next < 0) {
                    next = 0;
                } else if (next > 2) {
                    next = 2;
                }
                if (selection != next) {
                    selection = next;
                    render_mp3_actions(selection, last_status);
                }
            }
            if (key == 28 || key == 29) {
                unsigned char mode = Peek(MP3_CTRL_MODE);
                if (selection == 2) {
                    if (key == 28) {
                        mode = (mode >= MP3_PLAY_MODE_RANDOM) ? MP3_PLAY_MODE_SINGLE : (unsigned char)(mode + 1u);
                    } else {
                        mode = (mode == MP3_PLAY_MODE_SINGLE) ? MP3_PLAY_MODE_RANDOM : (unsigned char)(mode - 1u);
                    }
                    Poke(MP3_CTRL_MODE, mode);
                    render_mp3_actions(selection, last_status);
                    render_mp3_footer_line();
                }
            }
            if (key == 'n' || key == 'N' || key == 'p' || key == 'P') {
                unsigned char mode = Peek(MP3_CTRL_MODE);
                unsigned char status = Peek(MP3_CTRL_STATUS);
                if ((status & MP3_STATUS_PLAYING) && (mode == MP3_PLAY_MODE_ALL || mode == MP3_PLAY_MODE_RANDOM)) {
                    Poke(MP3_CTRL_CMD, (key == 'p' || key == 'P') ? MP3_CMD_PREVIOUS : MP3_CMD_NEXT);
                }
            }
            if (key == 'C' || key == 'c') {
                if (menu_ui_try_toggle_columns()) {
                    record = &records[detail_index % FILES_PER_PAGE];
                    render_mp3_screen(record);
                    render_mp3_counter_line();
                    render_mp3_actions(selection, last_status);
                    render_mp3_footer_line();
                }
            }
        }

        {
            unsigned int now = *jiffyPtr;
            if ((unsigned int)(now - last_counter_tick) >= MP3_COUNTER_POLL_JIFFIES) {
                unsigned int elapsed = read_mp3_elapsed();
                unsigned char status = Peek(MP3_CTRL_STATUS);
                unsigned int selected_index = read_mp3_selected_index();
                last_counter_tick = now;
                if (selected_index != last_detail_index && selected_index < totalFiles) {
                    detail_index = selected_index;
                    last_detail_index = selected_index;
                    record = load_mp3_detail_record(detail_index);
                    render_mp3_screen(record);
                    render_mp3_counter_line();
                    render_mp3_actions(selection, status);
                    render_mp3_footer_line();
                }
                if (elapsed != last_elapsed || status != last_status ||
                    (unsigned int)(now - last_force_tick) >= MP3_COUNTER_FORCE_JIFFIES) {
                    render_mp3_counter_line();
                    if (status != last_status) {
                        render_mp3_actions(selection, status);
                    }
                    last_elapsed = elapsed;
                    last_status = status;
                    last_force_tick = now;
                }
            }
        }
        delay_ms(10);
    }

    frame_rendered = 0;
    displayMenu();
}

static void build_mapper_text(const ROMRecord *record, int waiting_mapper, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    if (waiting_mapper) {
        strncpy(out, "Detecting...", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    unsigned char mapper_code = record_mapper_code(record->Mapper);
    if (mapper_code == 0) {
        strncpy(out, "Unknown mapper", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    const char *desc = mapper_description(record->Mapper);
    if (record_mapper_is_override(record->Mapper)) {
        strncpy(out, desc, out_size - 1);
        out[out_size - 1] = '\0';
    } else {
        size_t desc_len = strlen(desc);
        size_t suffix_len = strlen(" (detected)");
        size_t max_desc = out_size - 1;
        if (max_desc > suffix_len) {
            max_desc -= suffix_len;
        } else {
            max_desc = 0;
        }
        if (desc_len > max_desc) {
            desc_len = max_desc;
        }
        memcpy(out, desc, desc_len);
        out[desc_len] = '\0';
        if (out_size > 1 && desc_len + suffix_len < out_size) {
            memcpy(out + desc_len, " (detected)", suffix_len);
            out[desc_len + suffix_len] = '\0';
        }
    }
}

static unsigned char audio_profile_is_supported(const ROMRecord *record, unsigned char audio_profile) {
    if (current_wavegame_rom) {
        return audio_profile == AUDIO_PROFILE_NONE;
    }
    if (audio_profile == AUDIO_PROFILE_NONE) {
        return 1;
    }
    if (audio_profile >= AUDIO_PROFILE_SCC && audio_profile <= AUDIO_PROFILE_SCC_PLUS) {
        return record_supports_scc_audio(record);
    }
    if (audio_profile >= AUDIO_PROFILE_SCC_EXTERNAL && audio_profile <= AUDIO_PROFILE_SCC_PLUS_EXTERNAL) {
        return record_supports_external_scc_audio(record);
    }
    if (audio_profile >= AUDIO_PROFILE_MEGARAM_SCC && audio_profile <= AUDIO_PROFILE_MEGARAM_SCC_PLUS) {
        unsigned char mapper_code = record_mapper_code(record->Mapper);
        return mapper_code >= 19 && mapper_code <= 21;
    }
    if ((audio_profile >= AUDIO_PROFILE_YM2151_SFG05 && audio_profile <= AUDIO_PROFILE_YM2151_SFG01) ||
        (audio_profile >= AUDIO_PROFILE_YM2151_SFG05_4MHZ && audio_profile <= AUDIO_PROFILE_YM2151_SFG01_4MHZ)) {
        return record_supports_msx_music(record);
    }
    if (audio_profile == AUDIO_PROFILE_DUAL_PSG) {
        return record_supports_dual_psg(record);
    }
    if (audio_profile == AUDIO_PROFILE_MSX_MUSIC) {
        return record_supports_msx_music(record) && !record_is_sunrise_mapper_system_rom(record);
    }
    return 0;
}

static unsigned char default_audio_profile(const ROMRecord *record) {
    if (current_wavegame_rom) {
        return AUDIO_PROFILE_NONE;
    }
    return record_supports_scc_audio(record) ? AUDIO_PROFILE_SCC : AUDIO_PROFILE_NONE;
}

static unsigned char sanitize_audio_profile(const ROMRecord *record, unsigned char audio_profile) {
    if (audio_profile_is_supported(record, audio_profile)) {
        return audio_profile;
    }
    return default_audio_profile(record);
}

static unsigned char next_audio_profile(const ROMRecord *record, unsigned char audio_profile, int dir) {
    static const unsigned char audio_profiles[] = {
        AUDIO_PROFILE_NONE,
        AUDIO_PROFILE_SCC,
        AUDIO_PROFILE_SCC_PLUS,
        AUDIO_PROFILE_SCC_EXTERNAL,
        AUDIO_PROFILE_SCC_PLUS_EXTERNAL,
        AUDIO_PROFILE_MEGARAM_SCC,
        AUDIO_PROFILE_MEGARAM_SCC_PLUS,
        AUDIO_PROFILE_YM2151_SFG05,
        AUDIO_PROFILE_YM2151_SFG01,
        AUDIO_PROFILE_YM2151_SFG05_4MHZ,
        AUDIO_PROFILE_YM2151_SFG01_4MHZ,
        AUDIO_PROFILE_DUAL_PSG,
        AUDIO_PROFILE_MSX_MUSIC
    };
    const unsigned int audio_count = (unsigned int)(sizeof(audio_profiles) / sizeof(audio_profiles[0]));
    int current = -1;

    for (unsigned int i = 0; i < audio_count; i++) {
        if (audio_profiles[i] == audio_profile) {
            current = (int)i;
            break;
        }
    }
    if (current < 0) {
        current = 0;
    }

    for (unsigned int step = 0; step < audio_count; step++) {
        current += dir;
        if (current < 0) {
            current = (int)(audio_count - 1);
        } else if ((unsigned int)current >= audio_count) {
            current = 0;
        }
        if (audio_profile_is_supported(record, audio_profiles[current])) {
            return audio_profiles[current];
        }
    }

    return AUDIO_PROFILE_NONE;
}

static void build_audio_text(const ROMRecord *record, unsigned char audio_profile, char *out, size_t out_size) {
    const char *audio_label = "None";
    if (!out || out_size == 0) {
        return;
    }
    audio_profile = sanitize_audio_profile(record, audio_profile);
    if (audio_profile == AUDIO_PROFILE_SCC) {
        audio_label = "SCC";
    } else if (audio_profile == AUDIO_PROFILE_SCC_PLUS) {
        audio_label = "SCC+";
    } else if (audio_profile == AUDIO_PROFILE_SCC_EXTERNAL) {
        audio_label = "External SCC";
    } else if (audio_profile == AUDIO_PROFILE_SCC_PLUS_EXTERNAL) {
        audio_label = "External SCC+";
    } else if (audio_profile == AUDIO_PROFILE_MEGARAM_SCC) {
        audio_label = "MegaRAM SCC";
    } else if (audio_profile == AUDIO_PROFILE_MEGARAM_SCC_PLUS) {
        audio_label = "MegaRAM SCC+";
    } else if (audio_profile == AUDIO_PROFILE_YM2151_SFG05) {
        audio_label = "YM2164 SFG05";
    } else if (audio_profile == AUDIO_PROFILE_YM2151_SFG01) {
        audio_label = "YM2151 SFG01";
    } else if (audio_profile == AUDIO_PROFILE_YM2151_SFG05_4MHZ) {
        audio_label = "YM2164 SFG05 4MHZ";
    } else if (audio_profile == AUDIO_PROFILE_YM2151_SFG01_4MHZ) {
        audio_label = "YM2151 SFG01 4MHZ";
    } else if (audio_profile == AUDIO_PROFILE_DUAL_PSG) {
        audio_label = "Dual PSG";
    } else if (audio_profile == AUDIO_PROFILE_MSX_MUSIC) {
        audio_label = "FMPAC/MSX-MUSIC";
    }
    strncpy(out, audio_label, out_size - 1);
    out[out_size - 1] = '\0';
}
