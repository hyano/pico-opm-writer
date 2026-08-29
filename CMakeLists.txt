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

# ファームウェアのバージョンの唯一の定義場所。picotool のメタデータ・起動バナー・
# リリースパッケージはすべてここの VERSION から導出する（他の場所に書かない）。
project(pico-opm-writer VERSION 0.3.0 LANGUAGES C CXX ASM)

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
# src/ffconf.h が拾われる。
add_library(fatfs STATIC
        external/fatfs/ff.c
        external/fatfs/ffunicode.c
        )
target_include_directories(fatfs PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/external/fatfs
        ${CMAKE_CURRENT_LIST_DIR}/src
        )

# miniz も同じ理由で別ターゲットにする。使うのは展開器 tinfl だけなので、
# 圧縮側と zip アーカイブ側は MINIZ_NO_* で全部落とす。設定ヘッダを持たない
# 造りなので、ここが ffconf.h の代わりになる（external/README.md）。
add_library(miniz STATIC
        external/miniz/miniz.c
        )
target_include_directories(miniz PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/external/miniz
        )
target_compile_definitions(miniz PUBLIC
        MINIZ_NO_STDIO
        MINIZ_NO_TIME
        MINIZ_NO_MALLOC
        MINIZ_NO_DEFLATE_APIS
        MINIZ_NO_ARCHIVE_APIS
        MINIZ_NO_ZLIB_APIS
        )

add_executable(pico-opm-writer
        src/pico-opm-writer.c
        src/sched.c
        src/opm.c
        src/clockmode.c
        src/ym3012.c
        src/capture.c
        src/i2s.c
        src/usb_pcm.c
        src/led.c
        src/button.c
        src/buttonmap.c
        src/stats.c
        src/flash_disk.c
        src/diskio_flash.c
        src/storage.c
        src/filelist.c
        src/songend.c
        src/autoplay.c
        src/vgm.c
        src/vgz.c
        src/mdx.c
        src/pcm8.c
        src/usb_msc.c
        src/usb_descriptors.c
        )

pico_set_program_name(pico-opm-writer "pico-opm-writer")

# バイナリに埋め込むメタデータ（picotool info が読む）と、起動バナー / `i` が出す
# 版番号。どちらも上の project(VERSION) から出す。
pico_set_program_version(pico-opm-writer ${PROJECT_VERSION})
target_compile_definitions(pico-opm-writer PRIVATE
        OPM_WRITER_VERSION="${PROJECT_VERSION}")

# 自前のソースだけ警告を厳しくする（SDK 側のヘッダには適用しない）
target_compile_options(pico-opm-writer PRIVATE -Wall -Wextra)

# Generate PIO header
pico_generate_pio_header(pico-opm-writer ${CMAKE_CURRENT_LIST_DIR}/src/opm_clock.pio)
pico_generate_pio_header(pico-opm-writer ${CMAKE_CURRENT_LIST_DIR}/src/ym3012.pio)
pico_generate_pio_header(pico-opm-writer ${CMAKE_CURRENT_LIST_DIR}/src/i2s.pio)

# 起動時の φM プリセット（0 = 4MHz / 1 = 3.579545MHz）。未指定なら opm.h の既定値を使う。
# 実行時は clock コマンドと VGM の追従でどちらへも切り替えられる。
set(OPM_CLOCK_MODE "" CACHE STRING "boot phiM preset: 0 = 4MHz, 1 = 3.579545MHz")
if(NOT OPM_CLOCK_MODE STREQUAL "")
    target_compile_definitions(pico-opm-writer PRIVATE OPM_CLOCK_MODE=${OPM_CLOCK_MODE})
endif()

# I2S 出力（GP26-GP28）。ym3012.h のループバック自己診断の既定値もこれで決まる。
set(I2S_ENABLED "1" CACHE STRING "I2S output: 1 = enabled, 0 = disabled")
target_compile_definitions(pico-opm-writer PRIVATE I2S_ENABLED=${I2S_ENABLED})

# .vgz（gzip 圧縮された VGM）の再生。0 にすると展開器と約 84KiB の
# バッファがリンクされず、.vgz は bad file になる。
set(VGM_VGZ_ENABLED "1" CACHE STRING "gzip (.vgz) playback: 1 = enabled, 0 = disabled")
target_compile_definitions(pico-opm-writer PRIVATE VGM_VGZ_ENABLED=${VGM_VGZ_ENABLED})

# MDX（X68000 / MXDRV）の再生。0 にするとシーケンサと 64KB のファイルバッファが
# リンクされず、mdx コマンドは wrong state になる。
set(MDX_ENABLED "1" CACHE STRING "MDX (X68000/MXDRV) playback: 1 = enabled, 0 = disabled")
target_compile_definitions(pico-opm-writer PRIVATE MDX_ENABLED=${MDX_ENABLED})

# MDX の ADPCM パート（PCM8 相当 8ch）のソフトデコードとミキシング。
# 0 にするとデコーダとミックスリング（約 27KB）がリンクされず、
# ADPCM パートはシーケンサとして回るだけで鳴らなくなる。
# 空なら pcm8.h の既定（MDX_ENABLED に従う）を使う。
set(PCM8_ENABLED "" CACHE STRING "MDX ADPCM (PCM8) mixing: 1 = on, 0 = off, empty = follow MDX_ENABLED")
if(NOT PCM8_ENABLED STREQUAL "")
    target_compile_definitions(pico-opm-writer PRIVATE PCM8_ENABLED=${PCM8_ENABLED})
endif()

# VGM / MDX の自動連続再生。0 にするとプレイリスト（約 26KB）と状態機械が
# リンクされず、autoplay コマンドは unsupported になる。
set(AUTOPLAY_ENABLED "1" CACHE STRING "unattended VGM/MDX playback: 1 = enabled, 0 = disabled")
target_compile_definitions(pico-opm-writer PRIVATE AUTOPLAY_ENABLED=${AUTOPLAY_ENABLED})

# GP21 / GP22 のボタン操作（内部プルアップの負論理）。0 にすると状態機械が
# リンクされず、GP21 / GP22 は初期化もされないのでピンを他の用途へ回せる。
# スイッチを実装していない基板でも内部プルアップで「離されている」と読めるので、
# 既定の 1 のままで従来と同じ挙動になる。
set(BUTTON_ENABLED "1" CACHE STRING "GPIO buttons on GP21/GP22: 1 = enabled, 0 = disabled")
target_compile_definitions(pico-opm-writer PRIVATE BUTTON_ENABLED=${BUTTON_ENABLED})

# サービスごとの滞在時間の計測。1 にすると service_all()（src/sched.c）の各呼び出しと
# opm_write() が time_us_32() を 2 回読むようになり、`s` に SVCTIME 行（us/s）と
# OPMW 行（writes/s・us/s・1 回あたりの平均）が増える。
# 常設するには重いので、内訳を見たいときだけ 1 で焼く。
set(STATS_PROFILE "0" CACHE STRING "per-service timing profile: 1 = on, 0 = off")
target_compile_definitions(pico-opm-writer PRIVATE STATS_PROFILE=${STATS_PROFILE})

# FatFs 領域（内蔵フラッシュ）。通常は FLASH_FATFS_RESERVE_KB だけを指定する。
# ファームウェアに残す KiB を決めれば、そこから末尾の予約セクタの手前までが
# 自動で FatFs になる。オフセットとサイズをバイトで名指しすることもできる。
#
# 末尾の予約は picotool が UF2 へ入れる RP2350-E10 の absolute block 用。
# 0x10FFFF00 狙いのブロックが 4MiB のフラッシュでは末尾セクタへ折り返すので、
# ここを空けておかないと UF2 で焼くたびにファイルシステムの末尾が潰れる。
#
# FLASH_TOTAL_BYTES はここでの引き算（FatFs 領域の末尾）にしか使わない。
# PICO_FLASH_SIZE_BYTES を設定するわけではないので、SDK 側の値とずれていないかは
# flash_disk.c の _Static_assert がコンパイル時に検査する。
set(FLASH_TOTAL_BYTES "4194304" CACHE STRING "flash size in bytes used to size the FatFs region; must match PICO_FLASH_SIZE_BYTES (checked by _Static_assert in flash_disk.c)")
set(FLASH_FATFS_RESERVE_KB "" CACHE STRING "KiB of flash reserved for the firmware; FatFs takes the rest (default 256)")
set(FLASH_FATFS_TAIL_RESERVE "4096" CACHE STRING "bytes kept free at the end of flash (RP2350-E10 absolute block)")
set(FLASH_FATFS_OFFSET "" CACHE STRING "FatFs region offset from XIP_BASE in bytes (overrides FLASH_FATFS_RESERVE_KB)")
set(FLASH_FATFS_SIZE "" CACHE STRING "FatFs region size in bytes (default: fill up to the tail reserve)")

# 同じものを 2 通りで指定されると、どちらが効いたのか分からなくなる
if(NOT FLASH_FATFS_OFFSET STREQUAL "" AND NOT FLASH_FATFS_RESERVE_KB STREQUAL "")
    message(FATAL_ERROR
        "FLASH_FATFS_OFFSET と FLASH_FATFS_RESERVE_KB は同時に指定できない。"
        "どちらか一方にすること")
endif()

if(NOT FLASH_FATFS_OFFSET STREQUAL "")
    set(fatfs_offset ${FLASH_FATFS_OFFSET})
elseif(NOT FLASH_FATFS_RESERVE_KB STREQUAL "")
    math(EXPR fatfs_offset "${FLASH_FATFS_RESERVE_KB} * 1024")
else()
    math(EXPR fatfs_offset "256 * 1024") # flash_disk.h の既定と揃えること
endif()

if(NOT FLASH_FATFS_SIZE STREQUAL "")
    set(fatfs_size ${FLASH_FATFS_SIZE})
else()
    math(EXPR fatfs_size "${FLASH_TOTAL_BYTES} - ${FLASH_FATFS_TAIL_RESERVE} - ${fatfs_offset}")
endif()

# 詳しい検査は flash_disk.c の _Static_assert が行う。ここで見るのは、
# 引き算が負になって定義そのものが意味を成さなくなる場合だけ。
if(fatfs_size LESS_EQUAL 0)
    message(FATAL_ERROR
        "FatFs 領域のサイズが ${fatfs_size} バイトになった。"
        "FLASH_FATFS_RESERVE_KB が大きすぎる")
endif()

target_compile_definitions(pico-opm-writer PRIVATE
        FLASH_FATFS_OFFSET=${fatfs_offset}
        FLASH_FATFS_SIZE=${fatfs_size}
        FLASH_FATFS_TAIL_RESERVE=${FLASH_FATFS_TAIL_RESERVE})

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
        ${CMAKE_CURRENT_LIST_DIR}/src
)

# Add any user requested libraries
target_link_libraries(pico-opm-writer
        hardware_pio
        hardware_dma
        hardware_clocks
        hardware_pll
        hardware_flash
        pico_flash
        pico_rand
        tinyusb_device
        fatfs
        miniz
        )

pico_add_extra_outputs(pico-opm-writer)

# リンク結果が FatFs 領域に食い込んでいないかをビルド時に検査する。
# 実行時の storage_init() による検査は残してあるが、そちらは焼いて起動するまで
# 分からないので、こちらで先に落とす。
add_custom_command(TARGET pico-opm-writer POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DNM=${CMAKE_NM}
            -DELF=$<TARGET_FILE:pico-opm-writer>
            -DREGION_OFFSET=${fatfs_offset}
            -DREGION_SIZE=${fatfs_size}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_flash_region.cmake
        VERBATIM)


# エンドユーザ向けの配布 zip。`ninja -C build release` で
# build/release/pico-opm-writer-<version>.zip を作る。
#
# POST_BUILD にはしない。通常のビルドのたびに zip を組み直すのは無駄だし、
# git describe も毎回走ることになる。
#
# configure 時にしか分からない値（SDK のパス、各オプションの実効値）は
# release_config.cmake へ焼き、バージョンだけはパッケージ実行時に git から取り直す。
# 領域の位置は README §7.1 が 16 進で書いているので、両方を並べて出す
math(EXPR fatfs_offset_hex "${fatfs_offset}" OUTPUT_FORMAT HEXADECIMAL)

configure_file(
        ${CMAKE_CURRENT_LIST_DIR}/cmake/release_config.cmake.in
        ${CMAKE_BINARY_DIR}/release_config.cmake
        @ONLY)

add_custom_target(release
        COMMAND ${CMAKE_COMMAND}
            -DRELEASE_CONFIG=${CMAKE_BINARY_DIR}/release_config.cmake
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/make_release.cmake
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        COMMENT "packaging release zip"
        VERBATIM)
add_dependencies(release pico-opm-writer)
