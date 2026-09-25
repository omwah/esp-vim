# Keyboards as console input (include/esp_kbd.h). Outside the vim component:
# the queue outlives Vim sessions, and keyboard drivers feed it from their own
# tasks.
idf_component_register(SRCS "esp_kbd.c" INCLUDE_DIRS "include" PRIV_REQUIRES esp_timer heap)
