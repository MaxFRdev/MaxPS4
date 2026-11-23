#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Records an abort (SIGABRT, SIGFPE) or an uncaught Objective-C exception in
// Documents/maxps4-fermeture.txt before the app dies, with the host
// backtrace. Install once at launch.
void maxps4_crash_log_install(void);
#ifdef __cplusplus
}
#endif
