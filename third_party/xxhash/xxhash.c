/* xxHash implementation file.
 *
 * This file only defines XXH_STATIC_LINKING_ONLY + XXH_IMPLEMENTATION and
 * includes xxhash.h; the actual implementation lives in the header (it is
 * written that way upstream). Only xxhash.c + xxhash.h are vendored here;
 * xxhsum, xxh_x86dispatch.c, tests and docs are not taken.
 *
 * Sampling only uses XXH3-64 (see src/sampling.c).
 * License: BSD-2-Clause (see xxhash.h header comment).
 */
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include "xxhash.h"
