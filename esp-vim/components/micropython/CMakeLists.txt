# MicroPython as an ESP-IDF component, embedded in Vim (docs/PLAN.md, Phase 7).
#
# MicroPython's own CMake fragments do the work, as in its ESP32 port
# (ports/esp32/esp32_common.cmake): py/py.cmake lists the core, and
# py/mkrules.cmake generates the qstr, module and root-pointer tables at build
# time by preprocessing the sources (with Python, which ESP-IDF's environment
# has). The embed port (ports/embed) was not used: its generator copies py/
# alone, with none of extmod's modules.
#
# The configuration is port/mpconfigport.h; the port is port/esp_py.c (the
# interpreter's lifetime, running code, the _vim module) and port/mphal_esp.c
# (ESP-IDF: clock, sleep, stack, heap, GC roots).
#
# Source is NOT in this repo: it is extracted into build-deps/ by
# scripts/prepare-deps.sh.

cmake_minimum_required(VERSION 3.16)

get_filename_component(MICROPY_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../build-deps/micropython" ABSOLUTE)
if(NOT EXISTS "${MICROPY_DIR}/py/runtime.c")
    message(FATAL_ERROR
        "MicroPython source not found at ${MICROPY_DIR}.\n"
        "Run: pixi run deps")
endif()
set(MICROPY_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/port")

include(${MICROPY_DIR}/py/py.cmake)

set(MICROPY_SOURCE_EXTMOD
    ${MICROPY_DIR}/extmod/modbinascii.c
    ${MICROPY_DIR}/extmod/moddeflate.c
    ${MICROPY_DIR}/extmod/modhashlib.c
    ${MICROPY_DIR}/extmod/modheapq.c
    ${MICROPY_DIR}/extmod/modjson.c
    ${MICROPY_DIR}/extmod/modos.c
    ${MICROPY_DIR}/extmod/modrandom.c
    ${MICROPY_DIR}/extmod/modre.c
    ${MICROPY_DIR}/extmod/modtime.c
    ${MICROPY_DIR}/extmod/modvfs.c
    ${MICROPY_DIR}/extmod/vfs.c
    ${MICROPY_DIR}/extmod/vfs_posix.c
    ${MICROPY_DIR}/extmod/vfs_posix_file.c
    ${MICROPY_DIR}/extmod/vfs_reader.c
)
set(MICROPY_SOURCE_SHARED
    ${MICROPY_DIR}/shared/runtime/sys_stdio_mphal.c
    ${MICROPY_DIR}/shared/timeutils/timeutils.c
)
if(NOT CONFIG_IDF_TARGET_ARCH_XTENSA)
    # RISC-V: registers for the GC from C (Xtensa spills its windows instead,
    # in mphal_esp.c).
    list(APPEND MICROPY_SOURCE_SHARED ${MICROPY_DIR}/shared/runtime/gchelper_generic.c)
endif()
set(MICROPY_SOURCE_PORT ${MICROPY_PORT_DIR}/esp_py.c)
set(MICROPY_QSTRDEFS_PORT ${MICROPY_PORT_DIR}/qstrdefsport.h)

# Everything that may name a qstr. mphal_esp.c names none, and is left out.
set(MICROPY_SOURCE_QSTR
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_EXTMOD}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_PORT}
)

idf_component_register(
    SRCS ${MICROPY_SOURCE_PY} ${MICROPY_SOURCE_EXTMOD} ${MICROPY_SOURCE_SHARED}
         ${MICROPY_SOURCE_PORT} "port/mphal_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "port" "${MICROPY_DIR}" "${CMAKE_BINARY_DIR}"
    PRIV_REQUIRES mbedtls esp_timer esp_hw_support heap freertos
    LDFRAGMENTS "linker.lf"
)

set(MICROPY_TARGET ${COMPONENT_LIB})
set(MICROPY_ROM_TEXT_COMPRESSION ON)     # compressed error messages: less flash

# extmod's VFS code includes FatFs's header even with FAT off.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FFCONF_H=\"lib/oofatfs/ffconf.h\")
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-clobbered
    -Wno-deprecated-declarations
    -Wno-missing-field-initializers
    -Wno-unused-function
    -Wno-unused-variable)

# The qstr pass preprocesses with the target's own include paths and
# definitions (read by mkrules.cmake); hashlib's mbedTLS headers and the
# sdkconfig they read come from the rest of the build, gathered as the ESP32
# port gathers them.
foreach(comp ${__COMPONENT_NAMES_RESOLVED})
    micropy_gather_target_properties(__idf_${comp})
    micropy_gather_target_properties(${comp})
endforeach()
idf_build_get_property(config_dir CONFIG_DIR)
list(APPEND MICROPY_CPP_INC_EXTRA ${config_dir})
list(APPEND MICROPY_CPP_DEF_EXTRA "ESP_PLATFORM")

include(${MICROPY_DIR}/py/mkrules.cmake)
