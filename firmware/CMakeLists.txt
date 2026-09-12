cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Keep the zstd compatibility decoder small. New packs are gzip because gzip's
# bounded article-sized allocation is reliable on the C3; this remains for
# older v2 packs already installed in the field.
add_compile_definitions(ZSTD_DECODER_INTERNAL_BUFFER=1024)

# --- content partition images, rebuilt as part of `idf.py build` ------------
# Runs the host-side packer over the built-in article pack and wraps the
# archives in raw partition images. Fails the build if content exceeds its
# partition.
#
# Article text is CC BY-SA licensed and is not part of this repository. It
# lives in the companion pocketwiki-content repository: set
# POCKETWIKI_CONTENT_DIR to that checkout, or clone it next to this one.
set(PW_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(DEFINED ENV{POCKETWIKI_CONTENT_DIR})
    set(PW_CONTENT_DIR "$ENV{POCKETWIKI_CONTENT_DIR}")
else()
    set(PW_CONTENT_DIR "${PW_REPO_ROOT}/../pocketwiki-content")
endif()
set(PW_ARTICLES_DIR "${PW_CONTENT_DIR}/articles/db-packs/biology-health" CACHE PATH
    "Directory containing the articles packed into the built-in PocketWiki archive")
if(NOT EXISTS "${PW_ARTICLES_DIR}")
    message(FATAL_ERROR
        "Built-in article pack not found at ${PW_ARTICLES_DIR}.\n"
        "The article corpus is not part of this repository. Clone "
        "pocketwiki-content beside this checkout, or set POCKETWIKI_CONTENT_DIR "
        "to an existing one.")
endif()

# Match the partition layout to the flash size of the build target.
if(IDF_TARGET STREQUAL "esp32s3")
    set(PW_PARTITIONS_CSV "${CMAKE_CURRENT_SOURCE_DIR}/partitions_16mb.csv")
else()
    set(PW_PARTITIONS_CSV "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv")
endif()
set(PW_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/content")
set(PW_CONTENT_BIN "${PW_OUT_DIR}/content.bin")
set(PW_INDEX_BIN "${PW_OUT_DIR}/index.bin")
set(PW_CONTENT_PART "${PW_OUT_DIR}/content.part")
set(PW_INDEX_PART "${PW_OUT_DIR}/index.part")

file(GLOB_RECURSE PW_ARTICLE_FILES "${PW_ARTICLES_DIR}/*.md" "${PW_ARTICLES_DIR}/*.html" "${PW_ARTICLES_DIR}/*.htm")
file(GLOB PW_PACKER_FILES "${PW_REPO_ROOT}/tools/*.py")

add_custom_command(
    OUTPUT "${PW_CONTENT_BIN}" "${PW_INDEX_BIN}" "${PW_CONTENT_PART}" "${PW_INDEX_PART}"
    COMMAND "${CMAKE_COMMAND}" -E env
            python3 "${PW_REPO_ROOT}/tools/pack_content.py" build
            "${PW_ARTICLES_DIR}" "${PW_OUT_DIR}" --codec gzip
    COMMAND "${CMAKE_COMMAND}" -E env
            python3 "${PW_REPO_ROOT}/tools/pack_content.py" partition-image
            "${PW_OUT_DIR}" --partitions "${PW_PARTITIONS_CSV}"
    DEPENDS ${PW_ARTICLE_FILES} ${PW_PACKER_FILES} "${PW_PARTITIONS_CSV}"
    COMMENT "Packing articles into content/index partitions"
    VERBATIM)

add_custom_target(content_part ALL DEPENDS "${PW_CONTENT_BIN}" "${PW_INDEX_BIN}"
                                           "${PW_CONTENT_PART}" "${PW_INDEX_PART}")

project(pocketwiki)
