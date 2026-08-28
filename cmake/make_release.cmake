# エンドユーザ向けの配布 zip を組み立てる。
#
#   cmake -DRELEASE_CONFIG=<build>/release_config.cmake -P cmake/make_release.cmake
#
# 通常は CMakeLists.txt の release ターゲット経由で呼ばれる（ninja -C build release）。
#
# バージョン文字列はここで git から取り直す。configure 時に凍結すると、
# コミットが進んだあとも古い版のまま zip ができてしまうため。
#
# -DRELEASE_VERSION=<文字列> を渡すと git を見ずにその値を使う。

cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED RELEASE_CONFIG)
    message(FATAL_ERROR "release: RELEASE_CONFIG が指定されていない")
endif()
if(NOT EXISTS "${RELEASE_CONFIG}")
    message(FATAL_ERROR "release: ${RELEASE_CONFIG} が無い。再コンフィグすること")
endif()
include("${RELEASE_CONFIG}")

# 名前を width 桁まで空白で埋める。VERSION.txt の桁揃え用。
function(rel_pad out_var text width)
    string(LENGTH "${text}" len)
    set(padded "${text}")
    while(len LESS width)
        string(APPEND padded " ")
        math(EXPR len "${len} + 1")
    endwhile()
    set(${out_var} "${padded}" PARENT_SCOPE)
endfunction()

# git をリポジトリ上で実行する。失敗しても落とさず、出力を空にして res を返す。
function(rel_git out_var res_var)
    if(NOT GIT_EXECUTABLE)
        set(${out_var} "" PARENT_SCOPE)
        set(${res_var} 1 PARENT_SCOPE)
        return()
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} ${ARGN}
        WORKING_DIRECTORY "${REL_SOURCE_DIR}"
        OUTPUT_VARIABLE out
        RESULT_VARIABLE res
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out_var} "${out}" PARENT_SCOPE)
    set(${res_var} "${res}" PARENT_SCOPE)
endfunction()

find_program(GIT_EXECUTABLE git)

# ---------------------------------------------------------------- バージョン

if(DEFINED RELEASE_VERSION AND NOT RELEASE_VERSION STREQUAL "")
    set(describe "${RELEASE_VERSION}")
else()
    rel_git(describe describe_res describe --tags --always)
    if(NOT describe_res EQUAL 0 OR describe STREQUAL "")
        set(describe "${REL_FIRMWARE_VERSION}")
        message(WARNING
            "release: git describe --tags --always が使えないので、"
            "バージョンを ${describe} で代用する")
    endif()
endif()

# タグは release/<バージョン> の形式。zip のファイル名にスラッシュは使えないので落とす。
string(REGEX REPLACE "^release/" "" version "${describe}")

# タグ由来のときは、ファームウェアが名乗る版と一致していることを確かめる。
# ずれたまま通すと、zip 名は新しいのに焼いたファームは古い版を名乗る、という
# リリースができてしまう。
#
# タグが無い（短縮ハッシュ）とき、および -DRELEASE_VERSION= で名指ししたときは
# 照合しない。どちらもソースの版と一致する必然性がない。
if(describe MATCHES "^release/")
    # release/0.3.0-5-gabc1234 のような、タグから進んだ形の接尾辞を落とす
    string(REGEX REPLACE "-[0-9]+-g[0-9a-f]+$" "" tag_version "${version}")
    if(NOT tag_version STREQUAL "${REL_FIRMWARE_VERSION}")
        message(FATAL_ERROR
            "release: タグの版 \"${tag_version}\" とファームウェアの版 "
            "\"${REL_FIRMWARE_VERSION}\" が違う。"
            "CMakeLists.txt の project(VERSION) を直してコミットしてから、"
            "タグを打ち直すこと")
    endif()
endif()

if(version MATCHES "[^A-Za-z0-9._+-]")
    message(FATAL_ERROR
        "release: バージョン \"${version}\" にファイル名へ使えない文字が入っている。"
        "タグは release/<バージョン> の形式にすること")
endif()

rel_git(commit commit_res rev-parse HEAD)
if(NOT commit_res EQUAL 0)
    set(commit "unknown")
endif()

# 未追跡ファイルは数えない。ビルド結果に影響しないものまで dirty になる
rel_git(porcelain porcelain_res status --porcelain --untracked-files=no)
if(NOT porcelain_res EQUAL 0)
    set(tree_state "unknown")
elseif(porcelain STREQUAL "")
    set(tree_state "clean")
else()
    set(tree_state "dirty")
endif()

# ---------------------------------------------------------------- ステージング

set(release_dir "${REL_BINARY_DIR}/release")
set(stage_dir "${release_dir}/stage")
set(pkg_name "${REL_PROJECT_NAME}-${version}")
set(pkg_dir "${stage_dir}/${pkg_name}")
set(zip_path "${release_dir}/${pkg_name}.zip")

# 前回の中身が混ざらないよう毎回作り直す
file(REMOVE_RECURSE "${release_dir}")
file(MAKE_DIRECTORY "${pkg_dir}")

# src を dst へコピーする。src が無ければ落とす。
function(rel_copy src dst)
    if(NOT EXISTS "${src}")
        message(FATAL_ERROR "release: ${src} が無い")
    endif()
    configure_file("${src}" "${dst}" COPYONLY)
endfunction()

# ファームウェア本体
rel_copy("${REL_UF2}" "${pkg_dir}/${REL_PROJECT_NAME}.uf2")

# ドキュメントとライセンス
rel_copy("${REL_SOURCE_DIR}/README.md"  "${pkg_dir}/README.md")
rel_copy("${REL_SOURCE_DIR}/LICENSE"    "${pkg_dir}/LICENSE")
rel_copy("${REL_SOURCE_DIR}/cmake/release/THIRD-PARTY-LICENSES.md"
         "${pkg_dir}/THIRD-PARTY-LICENSES.md")

rel_copy("${REL_SOURCE_DIR}/docs/pico-opm-writer.md" "${pkg_dir}/docs/pico-opm-writer.md")
rel_copy("${REL_SOURCE_DIR}/docs/opm-writer.md"      "${pkg_dir}/docs/opm-writer.md")
rel_copy("${REL_SOURCE_DIR}/docs/opm-record.md"      "${pkg_dir}/docs/opm-record.md")

# ホスト側ツール（残り 2 本は実機調査の解析用なので入れない）
rel_copy("${REL_SOURCE_DIR}/tools/opm-writer.py" "${pkg_dir}/tools/opm-writer.py")
rel_copy("${REL_SOURCE_DIR}/tools/opm-record.py" "${pkg_dir}/tools/opm-record.py")

# 同梱した外部ソースのライセンス。README §12 がこの相対パスでリンクしているので、
# zip の中でもリンクが切れないよう配置をリポジトリと合わせる。
rel_copy("${REL_SOURCE_DIR}/external/README.md"          "${pkg_dir}/external/README.md")
rel_copy("${REL_SOURCE_DIR}/external/fatfs/LICENSE.txt"  "${pkg_dir}/external/fatfs/LICENSE.txt")
rel_copy("${REL_SOURCE_DIR}/external/miniz/LICENSE"      "${pkg_dir}/external/miniz/LICENSE")

# リンク時に取り込まれるがリポジトリには実体が無いもの。SDK から拾う。
# 落ちていたら黙って配らず落とす（バイナリ配布にはこの表示義務がある）。
rel_copy("${REL_PICO_SDK_PATH}/LICENSE.TXT"       "${pkg_dir}/licenses/pico-sdk-LICENSE.txt")
rel_copy("${REL_PICO_SDK_PATH}/lib/tinyusb/LICENSE" "${pkg_dir}/licenses/tinyusb-LICENSE")

# ---------------------------------------------------------------- VERSION.txt

string(TIMESTAMP build_time "%Y-%m-%dT%H:%M:%SZ" UTC)

# キーは英語（フィールド名なので）。値だけを並べ、説明は書かない。
function(rel_line out_var key value)
    rel_pad(padded "${key}:" 18)
    set(${out_var} "${${out_var}}${padded}${value}\n" PARENT_SCOPE)
endfunction()

set(v "")
rel_line(v "name"            "${REL_PROJECT_NAME}")
rel_line(v "version"         "${version}")
rel_line(v "describe"        "${describe}")
rel_line(v "commit"          "${commit}")
rel_line(v "tree"            "${tree_state}")
rel_line(v "built"           "${build_time}")
rel_line(v "firmware version" "${REL_FIRMWARE_VERSION}")
rel_line(v "board"           "${REL_PICO_BOARD}")
rel_line(v "platform"        "${REL_PICO_PLATFORM}")
rel_line(v "sdk"             "${REL_PICO_SDK_VERSION}")
rel_line(v "build type"      "${REL_BUILD_TYPE}")

string(APPEND v "\nbuild options:\n")

# REL_OPTIONS は名前と値が交互に並んだリスト。値が空の要素があるので、
# 展開は必ず IN LISTS で行う（${REL_OPTIONS} だと空要素が消えて対が 1 つずれる）。
set(opt_key "")
set(expect_key TRUE)
foreach(item IN LISTS REL_OPTIONS)
    if(expect_key)
        set(opt_key "${item}")
        set(expect_key FALSE)
    else()
        set(shown "${item}")
        if(shown STREQUAL "")
            set(shown "(default)")
        endif()
        rel_pad(padded "  ${opt_key}" 28)
        string(APPEND v "${padded}${shown}\n")
        set(expect_key TRUE)
    endif()
endforeach()
if(NOT expect_key)
    message(FATAL_ERROR "release: REL_OPTIONS の要素数が奇数 (${opt_key} に値が無い)")
endif()

string(APPEND v "\n")
rel_line(v "repository" "${REL_REPOSITORY_URL}")

file(WRITE "${pkg_dir}/VERSION.txt" "${v}")

# ---------------------------------------------------------------- SHA256SUMS

# 展開したディレクトリの中で `shasum -a 256 -c SHA256SUMS` が通る形式にする。
file(GLOB_RECURSE staged RELATIVE "${pkg_dir}" "${pkg_dir}/*")
list(SORT staged)

set(sums "")
foreach(rel IN LISTS staged)
    if(rel STREQUAL "SHA256SUMS")
        continue()
    endif()
    file(SHA256 "${pkg_dir}/${rel}" hash)
    string(APPEND sums "${hash}  ${rel}\n")
endforeach()
file(WRITE "${pkg_dir}/SHA256SUMS" "${sums}")

# ---------------------------------------------------------------- zip

# zip コマンドには依存しない。cmake -E tar が自前で書ける。
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar cf "../${pkg_name}.zip" --format=zip "${pkg_name}"
    WORKING_DIRECTORY "${stage_dir}"
    RESULT_VARIABLE tar_res)
if(NOT tar_res EQUAL 0)
    message(FATAL_ERROR "release: zip の作成に失敗した (${tar_res})")
endif()

file(SIZE "${zip_path}" zip_size)
message(STATUS "release: ${pkg_name}  (${describe}, ${tree_state})")
message(STATUS "         ${zip_path}  ${zip_size} B")

# GitHub Actions から呼ばれたときは、release/ を落としたあとのバージョンを
# ステップ出力として渡す。CI 側で同じ導出規則を書き直さずに済ませるため。
if(DEFINED ENV{GITHUB_OUTPUT} AND NOT "$ENV{GITHUB_OUTPUT}" STREQUAL "")
    file(APPEND "$ENV{GITHUB_OUTPUT}" "version=${version}\n")
    file(APPEND "$ENV{GITHUB_OUTPUT}" "zip=${zip_path}\n")
    file(APPEND "$ENV{GITHUB_OUTPUT}" "name=${pkg_name}\n")
endif()
