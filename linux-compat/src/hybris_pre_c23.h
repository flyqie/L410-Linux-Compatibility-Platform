/* Android 10 uses pre-C23 integer/scanf semantics. Keep those declarations
 * when cross-building on glibc 2.39 for the Debian 12 glibc 2.36 runtime.
 * This selects old public libc entry points; it does not change runtime ABI. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <features.h>
#undef __GLIBC_USE_ISOC2X
#define __GLIBC_USE_ISOC2X 0
#undef __GLIBC_USE_C2X_STRTOL
#define __GLIBC_USE_C2X_STRTOL 0
