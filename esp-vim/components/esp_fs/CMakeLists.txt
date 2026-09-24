# The one file-operations core (include/esp_fs.h). Deliberately NOT part of the
# vim component: the web server will call it from its own task, so it must not
# use Vim's allocator or globals. It is also outside the linker.lf brackets, so
# a Vim session restart leaves its mutex alone.
idf_component_register(
    SRCS "esp_fs.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs vfs heap freertos
)
