cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF's esp_hid, with our fixes (patches/esp-idf/): a copy of the component,
# patched at configure time, overrides the one in ESP-IDF by name. ESP-IDF
# itself is installed separately and never edited.
set(ESP_HID_PATCHES "${CMAKE_CURRENT_LIST_DIR}/../patches/esp-idf/0001-esp_hid-nimble-hidh-hid-service.patch")
file(SHA256 "${ESP_HID_PATCHES}" _esp_hid_sum)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ESP_HID_PATCHES}")
set(ESP_HID_COPY "${CMAKE_BINARY_DIR}/patched_components/esp_hid")
if(NOT EXISTS "${ESP_HID_COPY}/.patched-${_esp_hid_sum}")
    file(REMOVE_RECURSE "${ESP_HID_COPY}")
    file(COPY "$ENV{IDF_PATH}/components/esp_hid" DESTINATION "${CMAKE_BINARY_DIR}/patched_components")
    execute_process(COMMAND patch -p1 --forward --quiet -i "${ESP_HID_PATCHES}"
                    WORKING_DIRECTORY "${ESP_HID_COPY}" RESULT_VARIABLE _esp_hid_rc)
    if(NOT _esp_hid_rc EQUAL 0)
        message(FATAL_ERROR "Could not apply ${ESP_HID_PATCHES} to ESP-IDF's esp_hid "
                            "(this ESP-IDF is not the v5.5.5 it was made for?)")
    endif()
    file(TOUCH "${ESP_HID_COPY}/.patched-${_esp_hid_sum}")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${ESP_HID_COPY}")
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
# image per target, because :help names the chip it runs on.
set(VIMRT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build-deps/vimrt-${IDF_TARGET}")
if(NOT EXISTS "${VIMRT_DIR}/defaults.vim")
    message(FATAL_ERROR
        "Curated runtime not found at ${VIMRT_DIR}.\n"
        "Run: pixi run runtime")
endif()
fatfs_create_rawflash_image(vimrt "${VIMRT_DIR}" FLASH_IN_PROJECT)
