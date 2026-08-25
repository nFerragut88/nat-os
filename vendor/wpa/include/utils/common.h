/* nat-os shim: IDF crypto headers include "utils/common.h".
 * Redirected to the compatibility layer in vendor/wpa/include/includes.h
 * rather than vendoring IDF utils, which pull in a libc this kernel lacks. */
#ifndef WPA_UTILS_COMMON_SHIM_H
#define WPA_UTILS_COMMON_SHIM_H
#include "includes.h"
#endif
