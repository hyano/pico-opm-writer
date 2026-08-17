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
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico-opm-writer C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

# FatFs 本体（external/fatfs は上流のまま）。
#
# 別ターゲットにしてあるのは、下の -Wall -Wextra が PRIVATE で付いていて
# 上流コードに波及しないようにするため。これで警告抑止も改変も要らなくなる。
#
# ffconf.h は external/fatfs には置かない。ff.h の #include "ffconf.h" は
# まず ff.h 自身のあるディレクトリを探すので、そこに無いときだけ
# リポジトリ直下の ffconf.h が拾われる。
add_library(fatfs STATIC
        external/fatfs/ff.c
        external/fatfs/ffunicode.c
        )
target_include_directories(fatfs PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/external/fatfs
        ${CMAKE_CURRENT_LIST_DIR}
        )

add_executable(pico-opm-writer
        pico-opm-writer.c
        opm.c
        ym3012.c
        capture.c
        i2s.c
        usb_pcm.c
        led.c
        stats.c
        flash_disk.c
        diskio_flash.c
        storage.c
        vgm.c
        usb_msc.c
        usb_descriptors.c
        )

pico_set_program_name(pico-opm-writer "pico-opm-writer")
pico_set_program_version(pico-opm-writer "0.2")

# 自前のソースだけ警告を厳しくする（SDK 側のヘッダには適用しない）
target_compile_options(pico-opm-writer PRIVATE -Wall -Wextra)

# Generate PIO header
pico_generate_pio_header(pico-opm-writer ${CMAKE_CURRENT_LIST_DIR}/opm_clock.pio)
pico_generate_pio_header(pico-opm-writer ${CMAKE_CURRENT_LIST_DIR}/ym3012.pio)
pico_generate_pio_header(pico-opm-writer ${CMAKE_CURRENT_LIST_DIR}/i2s.pio)

# φM プリセット（0 = 4MHz / 1 = 3.579545MHz）。未指定なら opm.h の既定値を使う。
set(OPM_CLOCK_MODE "" CACHE STRING "phiM preset: 0 = 4MHz, 1 = 3.579545MHz")
if(NOT OPM_CLOCK_MODE STREQUAL "")
    target_compile_definitions(pico-opm-writer PRIVATE OPM_CLOCK_MODE=${OPM_CLOCK_MODE})
endif()

# I2S 出力（GP26-GP28）。ym3012.h のループバック自己診断の既定値もこれで決まる。
set(I2S_ENABLED "1" CACHE STRING "I2S output: 1 = enabled, 0 = disabled")
target_compile_definitions(pico-opm-writer PRIVATE I2S_ENABLED=${I2S_ENABLED})

# FatFs 領域（内蔵フラッシュ後半）。空なら flash_disk.h の既定値
# （オフセット 2MiB / サイズ 2MiB）を使う。fatfs 側にも同じ値を渡す。
set(FLASH_FATFS_OFFSET "" CACHE STRING "FatFs region offset from XIP_BASE, in bytes")
set(FLASH_FATFS_SIZE "" CACHE STRING "FatFs region size in bytes")
if(NOT FLASH_FATFS_OFFSET STREQUAL "")
    target_compile_definitions(pico-opm-writer PRIVATE FLASH_FATFS_OFFSET=${FLASH_FATFS_OFFSET})
endif()
if(NOT FLASH_FATFS_SIZE STREQUAL "")
    target_compile_definitions(pico-opm-writer PRIVATE FLASH_FATFS_SIZE=${FLASH_FATFS_SIZE})
endif()

# 起動時ループバック自己診断。空なら I2S_ENABLED から決める（I2S 有効時は無効）。
# I2S を配線したまま診断だけ試すことはできないので、DAC を外してから 1 を指定する。
set(YM3012_LOOPBACK "" CACHE STRING "boot loopback selftest: 1 = on, 0 = off, empty = auto")
if(NOT YM3012_LOOPBACK STREQUAL "")
    target_compile_definitions(pico-opm-writer PRIVATE YM3012_LOOPBACK_ENABLED=${YM3012_LOOPBACK})
endif()

# CDC #0（コマンド用）の printf がホスト未接続時にブロックする上限。
# 既定の 500000us（0.5 秒）のままだと、CDC #1 の PCM 出力用 DMA リング（16KB = 65.5ms 分）が
# ブロック中に溢れるため短くする。
target_compile_definitions(pico-opm-writer PRIVATE PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(pico-opm-writer 0)
pico_enable_stdio_usb(pico-opm-writer 1)

# Add the standard library to the build
target_link_libraries(pico-opm-writer
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(pico-opm-writer PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(pico-opm-writer
        hardware_pio
        hardware_dma
        hardware_clocks
        hardware_flash
        pico_flash
        tinyusb_device
        fatfs
        )

pico_add_extra_outputs(pico-opm-writer)

