#include "zova/cbm_zova_route.h"

#include "foundation/platform.h"
#include "zova/cbm_zova.h"

#include <stdio.h>

cbm_zova_route_t cbm_zova_route_from_env(void) {
    return cbm_zova_single_file_enabled()
               ? CBM_ZOVA_ROUTE_FULL_AUTHORITY
               : CBM_ZOVA_ROUTE_COMPATIBILITY;
}

cbm_zova_route_t cbm_zova_route_for_project(const char *project) {
    (void)project;
    return cbm_zova_route_from_env();
}

int cbm_zova_user_database_path(char *out, size_t out_size) {
    const char *cache = cbm_resolve_cache_dir();
    if (!out || out_size == 0 || !cache || !cache[0]) return -1;
    int n = snprintf(out, out_size, "%s/cbm.zova", cache);
    return n > 0 && (size_t)n < out_size ? 0 : -1;
}
