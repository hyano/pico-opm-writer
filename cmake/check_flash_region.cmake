# リンク結果と FatFs 領域が重なっていないかをビルド時に検査する。
#
# 実行時にも storage_init() が __flash_binary_end と突き合わせるが、そちらは
# 焼いて起動して storage status を見るまで分からない。ここで落としておけば
# 領域を詰めすぎたことがビルドの時点で分かる。
#
# 呼び出し側から NM / ELF / REGION_OFFSET / REGION_SIZE を -D で受け取る。

set(XIP_BASE 268435456) # 0x10000000

execute_process(
    COMMAND ${NM} ${ELF}
    OUTPUT_VARIABLE nm_out
    RESULT_VARIABLE nm_res
    ERROR_VARIABLE nm_err)

if(NOT nm_res EQUAL 0)
    message(FATAL_ERROR "flash: nm failed on ${ELF}: ${nm_err}")
endif()

# リンカが置く絶対シンボル。セクションの型は環境で揺れるので 1 文字なら何でも拾う
if(NOT nm_out MATCHES "([0-9a-fA-F]+) [A-Za-z] __flash_binary_end")
    message(FATAL_ERROR "flash: __flash_binary_end not found in ${ELF}")
endif()

math(EXPR fw_end_addr "0x${CMAKE_MATCH_1}")
math(EXPR fw_size "${fw_end_addr} - ${XIP_BASE}")
math(EXPR margin "${REGION_OFFSET} - ${fw_size}")
math(EXPR region_end "${REGION_OFFSET} + ${REGION_SIZE}")

math(EXPR region_hex "${REGION_OFFSET}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR region_end_hex "${region_end}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR fw_end_hex "${fw_end_addr}" OUTPUT_FORMAT HEXADECIMAL)

message(STATUS "flash: firmware ${fw_size} B (end ${fw_end_hex})")
message(STATUS "       FatFs  ${region_hex} + ${REGION_SIZE} B (end ${region_end_hex})")

if(margin LESS 0)
    math(EXPR overrun "0 - ${margin}")
    message(FATAL_ERROR
        "       firmware overruns the FatFs region by ${overrun} B. "
        "Raise FLASH_FATFS_RESERVE_KB (or shrink the firmware).")
endif()

message(STATUS "       margin ${margin} B  OK")
