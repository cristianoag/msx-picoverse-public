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
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Core2350B uses an RP2350B (48 GPIOs). The 'pico2' board is RP2350A
# (30 GPIOs only) and silently ignores any access to GPIO30..47, which
# is why GPIO38/39 used by the ESP-01 UART would not toggle. Use an
# RP2350B board definition so NUM_BANK0_GPIOS is 48.
set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")
# set(PICO_COPY_TO_RAM 1)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Fetch pico-extras if not available locally
set(PICO_EXTRAS_FETCH_FROM_GIT ON)
include(pico_extras_import.cmake)

project(loadrom C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# SD card library (no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
set(PICO_FATFS_PATH "${CMAKE_CURRENT_LIST_DIR}/lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico" CACHE PATH "Path to the FatFS/SD library")
add_subdirectory("${PICO_FATFS_PATH}/src" build_sd_lib)

# add_compile_options(-O3)

# Add executable. Default name is the project name, version 0.1
add_executable(loadrom 
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    loadrom.c 
    audio/emu2212.c
    audio/emu2149.c
    audio/emu2413.c
    memory/c2_emu.c
    storage/sunrise_ide.c
    storage/sunrise_sd.c
    storage/hw_config.c
    )

pico_generate_pio_header(loadrom ${CMAKE_CURRENT_LIST_DIR}/msx_bus.pio)
# wifi_uart.pio removed: the ESP-01 UART now uses the hardware UART1
# peripheral on GPIO38/39 (FUNCSEL=11=GPIO_FUNC_UART_AUX).

pico_set_program_name(loadrom "loadrom")
pico_set_program_version(loadrom "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(loadrom 0)
pico_enable_stdio_usb(loadrom 0)

# Add the standard library to the build
target_link_libraries(loadrom
    pico_stdlib
    pico_multicore
    pico_audio_i2s
    hardware_dma
    hardware_pio
    hardware_uart
    tinyusb_board
    tinyusb_host
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)

target_compile_definitions(loadrom PRIVATE
    PICO_AUDIO_I2S_PIO=0
)

# Add the standard include files to the build
target_include_directories(loadrom PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Set PICO_FLASH_SPI_CLKDIV
#pico_define_boot_stage2(loadrom_boot2  ${PICO_SDK_PATH}/src/rp2350/boot_stage2/compile_time_choice.S)
#target_compile_definitions(loadrom_boot2  PRIVATE PICO_FLASH_SPI_CLKDIV=3)
#pico_set_boot_stage2(loadrom loadrom_boot2)

pico_add_extra_outputs(loadrom)

# Custom command to copy files to dist folder
option(PICOVERSE_COPY_TO_DIST "Copy built firmware into the legacy dist directory" ON)
if(PICOVERSE_COPY_TO_DIST)
add_custom_command(TARGET loadrom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/loadrom.bin ${CMAKE_SOURCE_DIR}/dist/loadrom.bin
)
endif()
