cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-vim)

# Read-only $VIMRUNTIME image, flashed to the "vimrt" partition.
#
# Its contents are GENERATED, not committed: scripts/make-runtime-image.py copies
# a curated slice of Vim's own runtime out of build-deps/ and overlays our files
# from runtime-image/. Run it (pixi run runtime) before building.
set(VIMRT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build-deps/vimrt")
if(NOT EXISTS "${VIMRT_DIR}/defaults.vim")
    message(FATAL_ERROR
        "Curated runtime not found at ${VIMRT_DIR}.\n"
        "Run: pixi run runtime")
endif()
fatfs_create_rawflash_image(vimrt "${VIMRT_DIR}" FLASH_IN_PROJECT)
