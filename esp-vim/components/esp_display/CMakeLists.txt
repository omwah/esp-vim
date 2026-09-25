# The console on an LCD (include/esp_display.h). Built from esp_display.c, the
# libvterm that ships inside Vim's source (build-deps/vim/src/libvterm), and a
# font table generated from the vendored Spleen font (scripts/bdf2c.py).
set(VTERM "${CMAKE_CURRENT_LIST_DIR}/../../../build-deps/vim/src/libvterm")
set(FONT_BDF "${CMAKE_CURRENT_LIST_DIR}/../../../build-deps/spleen/spleen-6x12.bdf")
set(FONT_C "${CMAKE_CURRENT_BINARY_DIR}/esp_display_font.c")

if(CONFIG_ESP_VIM_DISPLAY)
    if(NOT EXISTS "${FONT_BDF}")
        message(FATAL_ERROR "Spleen font not found at ${FONT_BDF}. Run: pixi run deps")
    endif()
    idf_component_register(
        SRCS "esp_display.c"
             "${VTERM}/src/encoding.c" "${VTERM}/src/keyboard.c" "${VTERM}/src/mouse.c"
             "${VTERM}/src/parser.c" "${VTERM}/src/pen.c" "${VTERM}/src/screen.c"
             "${VTERM}/src/state.c" "${VTERM}/src/unicode.c" "${VTERM}/src/vterm.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "${VTERM}/include" "${VTERM}/src"
        PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_gpio heap log
    )
    add_custom_command(
        OUTPUT "${FONT_C}"
        COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/bdf2c.py" "${FONT_BDF}" "${FONT_C}"
        DEPENDS "${FONT_BDF}" "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/bdf2c.py"
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE "${FONT_C}")
    # As Vim's Makefile builds it, but with libvterm's own character-width
    # tables rather than Vim's: the display task must not call into Vim.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE INLINE=)
    # libvterm is upstream code, compiled as it is.
    set_source_files_properties(
        "${VTERM}/src/encoding.c" "${VTERM}/src/keyboard.c" "${VTERM}/src/mouse.c"
        "${VTERM}/src/parser.c" "${VTERM}/src/pen.c" "${VTERM}/src/screen.c"
        "${VTERM}/src/state.c" "${VTERM}/src/unicode.c" "${VTERM}/src/vterm.c"
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-function;-Wno-sign-compare;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wno-enum-conversion;-Wno-type-limits;-Wno-missing-field-initializers;-Wno-char-subscripts")
else()
    idf_component_register(SRCS "esp_display_stub.c" INCLUDE_DIRS "include")
endif()
