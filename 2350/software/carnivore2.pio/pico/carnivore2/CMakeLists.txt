# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Core2350B / PGA2350 uses an RP2350B (48 GPIOs) with 16 MB of flash. The 'pico2'
# board is RP2350A (30 GPIOs only) and silently ignores GPIO30..47.
set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Fetch pico-extras if not available locally (provides pico_audio_i2s)
set(PICO_EXTRAS_FETCH_FROM_GIT ON)
include(pico_extras_import.cmake)

project(carnivore2 C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# SD card library (no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
add_subdirectory(lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src build_sd_lib)

add_executable(carnivore2
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    carnivore2.c
    core/c2_core.c
    core/c2_flash.c
    core/c2_eeprom.c
    audio/c2_audio.c
    audio/emu2212.c
    audio/emu2149.c
    audio/emu2413.c
    storage/c2_store.c
    storage/sunrise_ide.c
    storage/sunrise_sd.c
    storage/hw_config.c
    )

pico_generate_pio_header(carnivore2 ${CMAKE_CURRENT_LIST_DIR}/msx_bus.pio)

pico_set_program_name(carnivore2 "carnivore2")
pico_set_program_version(carnivore2 "0.1")

# A copy_to_ram image delays the start of main() by the time needed to copy the
# firmware to SRAM, and a Panasonic FS-A1FX then missed the cartridge in the
# slot scan. Running from flash (default) starts fast; the bus path, the
# Carnivore2 core and the ROM cache are in RAM anyway. -DC2_COPY_TO_RAM=ON
# restores the SRAM image.
option(C2_COPY_TO_RAM "Run the firmware from SRAM" OFF)
set(C2_SYS_CLOCK_KHZ "300000" CACHE STRING "RP2350 system clock in kHz (210000 or 300000)")
if(C2_COPY_TO_RAM)
    pico_set_binary_type(carnivore2 copy_to_ram)
endif()

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(carnivore2 0)
pico_enable_stdio_usb(carnivore2 0)

target_link_libraries(carnivore2
    pico_stdlib
    pico_multicore
    pico_flash
    pico_audio_i2s
    hardware_dma
    hardware_flash
    hardware_pio
    tinyusb_board
    tinyusb_host
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)

target_compile_definitions(carnivore2 PRIVATE
    PICO_AUDIO_I2S_PIO=0
    # The Carnivore2 core runs from RAM: XIP is unavailable while flash is programmed.
    C2_RAM_FUNCS=1
    C2_SYS_CLOCK_KHZ=${C2_SYS_CLOCK_KHZ}u
)

target_include_directories(carnivore2 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(carnivore2)

# Custom command to copy files to dist folder
add_custom_command(TARGET carnivore2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/carnivore2.bin ${CMAKE_SOURCE_DIR}/dist/carnivore2.bin
)
