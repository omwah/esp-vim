cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF components with our fixes (patches/esp-idf/): a copy of each,
# patched at configure time, overrides the one in ESP-IDF by name. ESP-IDF
# itself is installed separately and never edited.
#   esp_hid  0001: the NimBLE HID host skipped the HID service
#   fatfs    0002: exFAT on, for SD cards
function(espvim_patched_component name patch)
    set(patch "${CMAKE_CURRENT_LIST_DIR}/../patches/esp-idf/${patch}")
    file(SHA256 "${patch}" sum)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    set(copy "${CMAKE_BINARY_DIR}/patched_components/${name}")
    if(NOT EXISTS "${copy}/.patched-${sum}")
        file(REMOVE_RECURSE "${copy}")
        file(COPY "$ENV{IDF_PATH}/components/${name}" DESTINATION "${CMAKE_BINARY_DIR}/patched_components")
        execute_process(COMMAND patch -p1 --forward --quiet -i "${patch}"
                        WORKING_DIRECTORY "${copy}" RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "Could not apply ${patch} to ESP-IDF's ${name} "
                                "(this ESP-IDF is not the v5.5.5 it was made for?)")
        endif()
        file(TOUCH "${copy}/.patched-${sum}")
    endif()
    set(EXTRA_COMPONENT_DIRS ${EXTRA_COMPONENT_DIRS} "${copy}" PARENT_SCOPE)
endfunction()
espvim_patched_component(esp_hid 0001-esp_hid-nimble-hidh-hid-service.patch)
espvim_patched_component(fatfs 0002-fatfs-exfat.patch)
# One component-manager lock file per build variant (ESP-IDF's documented way):
# the lock records the target and the Kconfig-dependent components, so a shared
# one is rewritten by every build of another variant. scripts/vim-build.sh sets
# ESPVIM_VARIANT (esp32p4, tab5, esp32s3).
if(NOT ESPVIM_VARIANT)
    set(ESPVIM_VARIANT ${IDF_TARGET})
endif()
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${ESPVIM_VARIANT})
project(esp-vim)

# Read-only $VIMRUNTIME image, flashed to the "vimrt" partition.
#
# Its contents are GENERATED, not committed: scripts/make-runtime-image.py copies
# a curated slice of Vim's own runtime out of build-deps/ and overlays our files
# from runtime-image/. Run it (pixi run runtime) before building. There is one
# image per build variant -- :help names the chip it runs on, and a narrow screen
# gets its help reflowed -- and a variant without its own (tab5) uses its chip's.
set(VIMRT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build-deps/vimrt-${ESPVIM_VARIANT}")
if(NOT EXISTS "${VIMRT_DIR}/defaults.vim")
    set(VIMRT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build-deps/vimrt-${IDF_TARGET}")
endif()
if(NOT EXISTS "${VIMRT_DIR}/defaults.vim")
    message(FATAL_ERROR
        "Curated runtime not found at ${VIMRT_DIR}.\n"
        "Run: pixi run runtime")
endif()
fatfs_create_rawflash_image(vimrt "${VIMRT_DIR}" FLASH_IN_PROJECT)
