#include "zova/cbm_zova_route.h"

#include "foundation/platform.h"

#include <stdio.h>

int cbm_zova_user_database_path(char *out, size_t out_size) {
    const char *cache = cbm_resolve_cache_dir();
    if (!out || out_size == 0 || !cache || !cache[0]) return -1;
    int n = snprintf(out, out_size, "%s/cbm.zova", cache);
    return n > 0 && (size_t)n < out_size ? 0 : -1;
}
