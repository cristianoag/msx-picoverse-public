# Generated Cmake Pico project file — standalone OPL4 / MoonSound firmware

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# RP2350B board with external PSRAM (matches the loadrom firmware).
set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Fetch pico-extras if not available locally (provides pico_audio_i2s)
set(PICO_EXTRAS_FETCH_FROM_GIT ON)
include(pico_extras_import.cmake)

project(opl4 C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

set(OPL4_TARGET_NAME opl4 CACHE STRING "Standalone OPL4 firmware target/output name")
set(OPL4_SAMPLE_RATE 44100 CACHE STRING "Standalone OPL4 I2S/emulation sample rate")
set(OPL4_OUTPUT_STEP 1 CACHE STRING "Standalone OPL4 emulated sample periods per generated output sample")

# Final output gain applied to the summed YMF278B output pins, as
# OPL4_OUTPUT_GAIN_NUM/2^OPL4_OUTPUT_GAIN_SHIFT. 96/32 = 3x puts the OPL4 at the
# same level as the SCC/PSG/MSX-MUSIC profiles (see the derivation in
# emu278b.cpp). Override only to A/B a different level.
set(OPL4_OUTPUT_GAIN_NUM 96 CACHE STRING "Standalone OPL4 output gain numerator")
set(OPL4_OUTPUT_GAIN_SHIFT 5 CACHE STRING "Standalone OPL4 output gain shift (denominator = 2^shift)")

# Set to 1 to enable USB CDC debug output, 0 to disable. Keep this off for
# normal timing validation; enabling it creates a USB CDC device and routes
# OPL4 diagnostics over USB stdio for field captures.
set(OPL4_USB_STDIO_DEBUG 0)

# Set to 0 to make ymfm clock every FM/PCM channel again, including silent ones,
# exactly as stock ymfm does. Only useful for A/B testing a suspected emulation
# difference against the default; =0 costs several times more CPU per sample.
set(OPL4_SKIP_INACTIVE_FM 1)
set(OPL4_SKIP_INACTIVE_PCM 1)

# Idle spins the read IRQ waits for before answering, while holding /WAIT. Raise
# only to A/B the pre-v2.72 behaviour (512 = ~150 us of Z80 stall per read).
set(OPL4_READ_SYNC_IDLE_SPINS 32)

add_executable(${OPL4_TARGET_NAME}
    opl4_main.c
    emu278b.cpp
    ymfm/ymfm_opl.cpp
    ymfm/ymfm_pcm.cpp
    ymfm/ymfm_adpcm.cpp
)

# The YMF278B synthesis (ymfm + the emu278b wrapper) is the single hottest path
# in the firmware and is right at the per-sample real-time budget under maximum
# polyphony. The Pico SDK Release build compiles at -O2; raise just these
# translation units to -O3 with loop unrolling for extra instruction-level
# throughput. This only changes optimization, not emulation behaviour - the
# audio output is bit-identical - so it is safe to apply to the audio core.
set_source_files_properties(
    emu278b.cpp
    ymfm/ymfm_opl.cpp
    ymfm/ymfm_pcm.cpp
    ymfm/ymfm_adpcm.cpp
    PROPERTIES COMPILE_OPTIONS "-O3;-funroll-loops"
)

# Dedicated PIO programs for the OPL4 I/O bus (read responder WITH /WAIT)
pico_generate_pio_header(${OPL4_TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/msx_opl4.pio)

pico_set_program_name(${OPL4_TARGET_NAME} "opl4")
pico_set_program_version(${OPL4_TARGET_NAME} "1.0")

# Run the whole firmware (including the ymfm OPL4 core) from SRAM. ymfm needs
# ~one full 210 MHz core to synthesise the YMF278B at 44.1 kHz, and when it
# executes from flash its instruction fetches share the XIP cache with the wave
# sample reads (YRW801 ROM in flash, PCM sample RAM in PSRAM). Under PCM load
# that contention dropped real-time throughput below 44.1 kHz. Copying code to
# RAM frees the XIP cache for wave-sample data only. The 2 MB YRW801 ROM and
# 2 MB PSRAM sample RAM are still accessed as data via XIP, which is unaffected.
pico_set_binary_type(${OPL4_TARGET_NAME} copy_to_ram)

# Keep standalone OPL4 timing tests free of USB CDC debug overhead by default.
pico_enable_stdio_uart(${OPL4_TARGET_NAME} 0)
pico_enable_stdio_usb(${OPL4_TARGET_NAME} ${OPL4_USB_STDIO_DEBUG})

target_link_libraries(${OPL4_TARGET_NAME}
    pico_stdlib
    pico_multicore
    pico_audio_i2s
    hardware_dma
    hardware_pio
)

if (OPL4_USB_STDIO_DEBUG)
    target_link_libraries(${OPL4_TARGET_NAME} tinyusb_board)
endif()

target_compile_definitions(${OPL4_TARGET_NAME} PRIVATE
    PICO_AUDIO_I2S_PIO=0
    OPL4_SAMPLE_RATE=${OPL4_SAMPLE_RATE}
    OPL4_OUTPUT_STEP=${OPL4_OUTPUT_STEP}
    OPL4_OUTPUT_GAIN_NUM=${OPL4_OUTPUT_GAIN_NUM}
    OPL4_OUTPUT_GAIN_SHIFT=${OPL4_OUTPUT_GAIN_SHIFT}
    OPL4_SKIP_INACTIVE_FM=${OPL4_SKIP_INACTIVE_FM}
    OPL4_SKIP_INACTIVE_PCM=${OPL4_SKIP_INACTIVE_PCM}
    OPL4_READ_SYNC_IDLE_SPINS=${OPL4_READ_SYNC_IDLE_SPINS}u
    OPL4_USB_DEBUG=$<BOOL:${OPL4_USB_STDIO_DEBUG}>
)

if (OPL4_USB_STDIO_DEBUG)
    target_compile_definitions(${OPL4_TARGET_NAME} PRIVATE
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=5000
        PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=500
        PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
        PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000
        USBD_PID=0x00E1
        USBD_PRODUCT="PicoVerse USB Debug CDC"
    )
endif()

# loadrom.h (shared pin map + PSRAM constants) lives in the loadrom firmware.
target_include_directories(${OPL4_TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../loadrom
)

pico_add_extra_outputs(${OPL4_TARGET_NAME})

# Copy firmware binary to dist folder for the tool to embed
add_custom_command(TARGET ${OPL4_TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/${OPL4_TARGET_NAME}.bin ${CMAKE_SOURCE_DIR}/dist/${OPL4_TARGET_NAME}.bin
)
