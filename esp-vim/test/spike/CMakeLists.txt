cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vim_spike)

# Read-only FAT image for /vimrt, built from vimrt_content/ and flashed with the
# project. This is the partition Vim will source $VIMRUNTIME from, so the spike
# has to prove it mounts and reads.
fatfs_create_rawflash_image(vimrt ${CMAKE_CURRENT_SOURCE_DIR}/vimrt_content FLASH_IN_PROJECT)
