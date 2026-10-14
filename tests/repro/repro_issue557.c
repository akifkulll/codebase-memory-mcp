/*
 * repro_issue557.c -- Regression guard for bug #557.
 *
 * Issue: #557 -- "cbm v0.8.1 silently deletes project DBs on 'corrupt'
 *                detection -- data loss with no recovery"
 *
 * The original destroying path unlinked a per-project index that failed an
 * integrity check, with no rename and no backup. Projects now live in the
 * shared <cache>/cbm.zova, so the guard is: a query against a cbm.zova that
 * cannot be read must leave the file in place.
 *
 * TRIGGER:
 *   1. Point CBM_CACHE_DIR at a temp directory.
 *   2. Write bytes that are not a database to <cache>/cbm.zova.
 *   3. Call search_graph for a project through the MCP handler.
 *   4. Assert that cbm.zova still exists with its original contents.
 */

#include <foundation/compat.h>
#include "test_framework.h"
#include "test_helpers.h"

#include <mcp/mcp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Project name used throughout: must pass cbm_validate_project_name(). */
#define REPRO557_PROJECT "cbm-repro557-test"
#define REPRO557_GARBAGE "not a zova database\n"

TEST(repro_issue557_corrupt_db_not_silently_deleted) {
    char tmp_cache[512];
    snprintf(tmp_cache, sizeof(tmp_cache), "/tmp/cbm_repro557_XXXXXX");
    if (!cbm_mkdtemp(tmp_cache)) {
        ASSERT_NOT_NULL(NULL); /* marks setup failure clearly */
    }
#if defined(_WIN32)
    char ev[600];
    snprintf(ev, sizeof(ev), "CBM_CACHE_DIR=%s", tmp_cache);
    _putenv(ev);
#else
    setenv("CBM_CACHE_DIR", tmp_cache, 1 /* overwrite */);
#endif

    char zova_path[700];
    snprintf(zova_path, sizeof(zova_path), "%s/cbm.zova", tmp_cache);
    th_write_file(zova_path, REPRO557_GARBAGE);

    cbm_mcp_server_t *srv = cbm_mcp_server_new();
    ASSERT_NOT_NULL(srv);
    char args[512];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"query\":\"Function\",\"limit\":1}",
             REPRO557_PROJECT);
    /* The response is an error; only the side effect on cbm.zova matters. */
    free(cbm_mcp_handle_tool(srv, "search_graph", args));
    cbm_mcp_server_free(srv);

    char contents[64] = {0};
    FILE *f = fopen(zova_path, "rb");
    if (f) {
        (void)fread(contents, 1, sizeof(contents) - 1, f);
        fclose(f);
    }

    th_rmtree(tmp_cache);
#if defined(_WIN32)
    _putenv("CBM_CACHE_DIR=");
#else
    unsetenv("CBM_CACHE_DIR");
#endif

    ASSERT_STR_EQ(contents, REPRO557_GARBAGE);
    PASS();
}

SUITE(repro_issue557) {
    RUN_TEST(repro_issue557_corrupt_db_not_silently_deleted);
}
