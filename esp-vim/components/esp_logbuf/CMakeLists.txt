# ESP-IDF's log, kept off the console while Vim has it (include/esp_logbuf.h).
# Its own component: the buffer must outlive Vim sessions, whose .bss is reset.
idf_component_register(SRCS "esp_logbuf.c" INCLUDE_DIRS "include"
    PRIV_REQUIRES log heap)
