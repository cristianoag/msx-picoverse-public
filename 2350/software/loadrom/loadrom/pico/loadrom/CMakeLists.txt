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
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_COPY_TO_RAM 1)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(loadrom C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# add_compile_options(-O3)

# Add executable. Default name is the project name, version 0.1
add_executable(loadrom 
    loadrom.c 
    msx_capture_addr.pio
    msx_output_data.pio
    )

pico_generate_pio_header(loadrom ${CMAKE_CURRENT_SOURCE_DIR}/msx_capture_addr.pio)
pico_generate_pio_header(loadrom ${CMAKE_CURRENT_SOURCE_DIR}/msx_output_data.pio)

pico_set_program_name(loadrom "loadrom")
pico_set_program_version(loadrom "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(loadrom 0)
pico_enable_stdio_usb(loadrom 1)

# Add the standard library to the build
target_link_libraries(loadrom
        hardware_pio
        hardware_dma
        pico_stdlib)

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
add_custom_command(TARGET loadrom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/loadrom.bin ${CMAKE_SOURCE_DIR}/dist/loadrom.bin
)