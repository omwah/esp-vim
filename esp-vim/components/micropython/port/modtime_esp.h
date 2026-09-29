/*
 * The time module's clock (included by extmod/modtime.c).
 *
 * As in MicroPython's ESP32 port, localtime() and gmtime() are both UTC; Vim's
 * strftime() gives local time, with the time zone set by :EspTime.
 */

#include <sys/time.h>

#include "py/obj.h"
#include "shared/timeutils/timeutils.h"

static void mp_time_localtime_get(timeutils_struct_time_t *tm) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    timeutils_seconds_since_epoch_to_struct_time(tv.tv_sec, tm);
}

static mp_obj_t mp_time_time_get(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return timeutils_obj_from_timestamp(tv.tv_sec);
}
