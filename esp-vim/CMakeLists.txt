cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-vim)

# Read-only $VIMRUNTIME image. Built from runtime-image/, which holds only files
# we author -- the curated slice of Vim's own runtime is copied in by
# scripts/make-runtime-image.sh (Phase 4).
fatfs_create_rawflash_image(vimrt ${CMAKE_CURRENT_SOURCE_DIR}/runtime-image FLASH_IN_PROJECT)
