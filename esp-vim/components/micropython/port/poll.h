/* ESP-IDF keeps poll() in sys/poll.h only (extmod/vfs_posix_file.c wants poll.h). */
#pragma once
#include <sys/poll.h>
