# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 13_3_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(loadrom C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(loadrom 
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    loadrom.c 
    sunrise_ide.c )

pico_generate_pio_header(loadrom ${CMAKE_CURRENT_LIST_DIR}/msx_bus.pio)

pico_set_program_name(loadrom "loadrom")
pico_set_program_version(loadrom "1.0")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(loadrom 0)
pico_enable_stdio_usb(loadrom 0)

# Add the standard library to the build
target_link_libraries(loadrom
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
        tinyusb_board
        tinyusb_host)

# Add the standard include files to the build
target_include_directories(loadrom PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
)

# Set PICO_FLASH_SPI_CLKDIV
#pico_define_boot_stage2(loadrom_boot2  ${PICO_SDK_PATH}/src/rp2040/boot_stage2/compile_time_choice.S)
#target_compile_definitions(loadrom_boot2  PRIVATE PICO_FLASH_SPI_CLKDIV=2)
#pico_set_boot_stage2(loadrom loadrom_boot2)

pico_add_extra_outputs(loadrom)

# Custom command to copy files to dist folder
option(PICOVERSE_COPY_TO_DIST "Copy built firmware into the legacy dist directory" ON)
if(PICOVERSE_COPY_TO_DIST)
add_custom_command(TARGET loadrom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/loadrom.bin ${CMAKE_SOURCE_DIR}/dist/loadrom.bin
)
endif()
