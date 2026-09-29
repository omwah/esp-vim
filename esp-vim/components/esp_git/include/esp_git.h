/*
 * esp_git: git on the device, over libgit2.
 *
 * For now only the stage 6z feasibility self-test (docs/PLAN.md): with
 * CONFIG_ESP_VIM_GIT_SELFTEST, a task waits for /fat/gittest.conf and runs
 * the steps it names, printing one "GIT-ST <step> ok|FAIL ..." line each.
 */

#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_ESP_VIM_GIT_SELFTEST
void esp_git_selftest_start(void);
#else
static inline void esp_git_selftest_start(void) {}
#endif

#ifdef __cplusplus
}
#endif
