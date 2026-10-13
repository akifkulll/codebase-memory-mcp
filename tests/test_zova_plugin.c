#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "cypher/cypher.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_plugin.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if !CBM_WITH_ZOVA

TEST(zova_plugin_disabled) {
    ASSERT_NULL(cbm_zova_plugin_descriptor());
    PASS();
}

#else

#include "zova.h"
#include "zova_plugin.h"

#include <yyjson/yyjson.h>

enum { TZP_PATH_MAX = 512, TZP_ROWS_MAX = 16, TZP_TEXT_MAX = 64 };

typedef struct {
    char path[TZP_PATH_MAX];
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    zova_database *db;
} plugin_fixture_t;

typedef struct {
    int64_t key;
    int hop;
    char name[TZP_TEXT_MAX];
    char file_path[TZP_TEXT_MAX];
    char properties[TZP_TEXT_MAX];
} plugin_hit_t;

/* Alpha -CALLS-> Beta -CALLS-> Gamma -CALLS-> Alpha,
 * Alpha -HTTP_CALLS-> Delta -ASYNC_CALLS-> Epsilon.
 * Alpha calls Beta twice (local names Beta and BetaAlias): one graph edge
 * carrying two cbm edges. */
static int plugin_publish(plugin_fixture_t *fx) {
    static const char *const names[] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon"};
    static const char *const files[] = {"alpha.c", "beta.c", "gamma.c", "delta.c", "epsilon.c"};
    static const char *const qns[] = {"proj.Alpha", "proj.Beta", "proj.Gamma", "proj.Delta",
                                      "proj.Epsilon"};
    CBMDumpNode nodes[5];
    memset(nodes, 0, sizeof(nodes));
    for (int i = 0; i < 5; i++) {
        nodes[i] = (CBMDumpNode){.id = i + 1, .project = "proj", .label = "Function",
                                 .name = names[i], .qualified_name = qns[i],
                                 .file_path = files[i], .start_line = 1, .end_line = 2,
                                 .properties = i == 1 ? "{\"k\":1}" : "{}"};
    }
    const CBMDumpEdge edges[] = {
        {.id = 1, .project = "proj", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{\"args\":\"x\"}", .local_name = "Beta"},
        {.id = 6, .project = "proj", .source_id = 1, .target_id = 2, .type = "CALLS",
         .properties = "{\"args\":\"y\"}", .local_name = "BetaAlias"},
        {.id = 2, .project = "proj", .source_id = 2, .target_id = 3, .type = "CALLS",
         .properties = "{}"},
        {.id = 3, .project = "proj", .source_id = 3, .target_id = 1, .type = "CALLS",
         .properties = "{}"},
        {.id = 4, .project = "proj", .source_id = 1, .target_id = 4, .type = "HTTP_CALLS",
         .properties = "{}"},
        {.id = 5, .project = "proj", .source_id = 4, .target_id = 5, .type = "ASYNC_CALLS",
         .properties = "{}"},
    };
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/plugin-trace", .project = "proj",
        .indexed_at = "2026-10-13T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
        .nodes = nodes, .node_count = 5, .edges = edges, .edge_count = 6,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    if (cbm_zova_user_database_publish_workspace(fx->path, &input, &published) != 0) return -1;
    return cbm_zova_workspace_graph_name(published.workspace_id, fx->graph_name,
                                         sizeof(fx->graph_name));
}

static zova_status plugin_open(const char *path, int with_plugin, zova_database **out_db) {
    const struct zova_plugin_descriptor_v1 *plugins[] = {cbm_zova_plugin_descriptor()};
    zova_message message = {0};
    zova_status status = zova_database_open_with_plugins(&(zova_database_open_plugins_request){
        .path = path, .plugins = plugins, .plugin_count = with_plugin ? 1 : 0,
        .out_db = out_db, .out_error_message = &message});
    zova_message_free(&message);
    return status;
}

static int plugin_fixture_publish(plugin_fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    snprintf(fx->path, sizeof(fx->path), "%s/cbm-plugin-%d-%p.zova", cbm_tmpdir(), (int)getpid(),
             (void *)fx);
    cbm_unlink(fx->path);
    return plugin_publish(fx);
}

static int plugin_fixture_install(plugin_fixture_t *fx) {
    if (plugin_open(fx->path, 1, &fx->db) != ZOVA_OK) return -1;
    return zova_database_extension_install(&(zova_database_extension_request){
               .db = fx->db, .name = "cbm"}) == ZOVA_OK ? 0 : -1;
}

static int plugin_fixture_create(plugin_fixture_t *fx) {
    return plugin_fixture_publish(fx) == 0 ? plugin_fixture_install(fx) : -1;
}

static void plugin_fixture_cleanup(plugin_fixture_t *fx) {
    if (fx->db) zova_database_close(fx->db);
    cbm_unlink(fx->path);
}

static void plugin_copy_text(zova_statement *stmt, int index, char *out, size_t out_size) {
    zova_text text = {0};
    out[0] = '\0';
    if (zova_statement_column_text(&(zova_statement_column_text_request){
            .statement = stmt, .index = index, .out_text = &text}) != ZOVA_OK) return;
    snprintf(out, out_size, "%.*s", (int)text.len, text.data ? text.data : "");
    zova_text_free(&text);
}

static int64_t plugin_node_key(const plugin_fixture_t *fx, const char *qualified_name) {
    zova_statement *stmt = NULL;
    int64_t key = 0;
    zova_step_result step = 0;
    if (zova_database_prepare(&(zova_database_prepare_request){
            .db = fx->db, .sql = "SELECT zova_node_key FROM cbm_nodes_v1 WHERE qualified_name=?1",
            .out_statement = &stmt}) != ZOVA_OK) return 0;
    if (zova_statement_bind_text(&(zova_statement_bind_text_request){
            .statement = stmt, .index = 1, .data = (const uint8_t *)qualified_name,
            .len = strlen(qualified_name)}) == ZOVA_OK &&
        zova_statement_step(&(zova_statement_step_request){
            .statement = stmt, .out_result = &step}) == ZOVA_OK && step == ZOVA_STEP_ROW) {
        (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = 0, .out_value = &key});
    }
    zova_statement_finalize(stmt);
    return key;
}

/* Returns the row count, or -1 when the statement fails. */
static int plugin_trace(const plugin_fixture_t *fx, int64_t start_key, const char *direction,
                        const char *edge_types, int max_depth, int max_results,
                        plugin_hit_t *hits) {
    zova_statement *stmt = NULL;
    if (zova_database_prepare(&(zova_database_prepare_request){
            .db = fx->db,
            .sql = "SELECT node_key,hop,name,file_path,properties "
                   "FROM zova_cbm_trace_path(?1,?2,?3,?4,?5,?6)",
            .out_statement = &stmt}) != ZOVA_OK) return -1;
    const char *texts[] = {fx->graph_name, direction, edge_types};
    const int text_index[] = {1, 3, 4};
    const int64_t ints[] = {start_key, max_depth, max_results};
    const int int_index[] = {2, 5, 6};
    int count = 0;
    for (int i = 0; i < 3 && count == 0; i++) {
        if (zova_statement_bind_text(&(zova_statement_bind_text_request){
                .statement = stmt, .index = text_index[i], .data = (const uint8_t *)texts[i],
                .len = strlen(texts[i])}) != ZOVA_OK ||
            zova_statement_bind_int64(&(zova_statement_bind_int64_request){
                .statement = stmt, .index = int_index[i], .value = ints[i]}) != ZOVA_OK) {
            count = -1;
        }
    }
    while (count >= 0 && count < TZP_ROWS_MAX) {
        zova_step_result step = 0;
        if (zova_statement_step(&(zova_statement_step_request){
                .statement = stmt, .out_result = &step}) != ZOVA_OK) {
            count = -1;
            break;
        }
        if (step != ZOVA_STEP_ROW) break;
        plugin_hit_t *hit = &hits[count++];
        int64_t hop = 0;
        (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = 0, .out_value = &hit->key});
        (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = 1, .out_value = &hop});
        hit->hop = (int)hop;
        plugin_copy_text(stmt, 2, hit->name, sizeof(hit->name));
        plugin_copy_text(stmt, 3, hit->file_path, sizeof(hit->file_path));
        plugin_copy_text(stmt, 4, hit->properties, sizeof(hit->properties));
    }
    zova_statement_finalize(stmt);
    return count;
}

TEST(zova_plugin_trace_path_follows_one_edge_type) {
    plugin_fixture_t fx;
    ASSERT_EQ(plugin_fixture_create(&fx), 0);
    int64_t alpha = plugin_node_key(&fx, "proj.Alpha");
    int64_t gamma = plugin_node_key(&fx, "proj.Gamma");
    ASSERT_GT(alpha, 0);
    ASSERT_GT(gamma, 0);
    plugin_hit_t hits[TZP_ROWS_MAX];

    /* The cycle back to Alpha is not reported and does not loop. */
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "CALLS", 5, 20, hits), 2);
    ASSERT_STR_EQ(hits[0].name, "Beta");
    ASSERT_EQ(hits[0].hop, 1);
    ASSERT_EQ(hits[0].key, plugin_node_key(&fx, "proj.Beta"));
    ASSERT_STR_EQ(hits[0].file_path, "beta.c");
    ASSERT_STR_EQ(hits[0].properties, "{\"k\":1}");
    ASSERT_STR_EQ(hits[1].name, "Gamma");
    ASSERT_EQ(hits[1].hop, 2);

    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "CALLS", 1, 20, hits), 1);
    ASSERT_STR_EQ(hits[0].name, "Beta");

    ASSERT_EQ(plugin_trace(&fx, gamma, "inbound", "CALLS", 5, 20, hits), 2);
    ASSERT_STR_EQ(hits[0].name, "Beta");
    ASSERT_EQ(hits[0].hop, 1);
    ASSERT_STR_EQ(hits[1].name, "Alpha");
    ASSERT_EQ(hits[1].hop, 2);

    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "NO_SUCH_TYPE", 5, 20, hits), 0);
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "CALLS", 0, 20, hits), 0);
    plugin_fixture_cleanup(&fx);
    PASS();
}

TEST(zova_plugin_trace_path_follows_edge_type_set) {
    plugin_fixture_t fx;
    ASSERT_EQ(plugin_fixture_create(&fx), 0);
    int64_t alpha = plugin_node_key(&fx, "proj.Alpha");
    plugin_hit_t hits[TZP_ROWS_MAX];

    /* Ordered by hop, then file path. */
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "CALLS,HTTP_CALLS,ASYNC_CALLS", 5, 20, hits),
              4);
    ASSERT_STR_EQ(hits[0].name, "Beta");
    ASSERT_EQ(hits[0].hop, 1);
    ASSERT_STR_EQ(hits[1].name, "Delta");
    ASSERT_EQ(hits[1].hop, 1);
    ASSERT_STR_EQ(hits[2].name, "Epsilon");
    ASSERT_EQ(hits[2].hop, 2);
    ASSERT_STR_EQ(hits[3].name, "Gamma");
    ASSERT_EQ(hits[3].hop, 2);

    /* A name the graph does not have matches nothing; the rest still apply. */
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "HTTP_CALLS,NO_SUCH_TYPE", 5, 20, hits), 1);
    ASSERT_STR_EQ(hits[0].name, "Delta");

    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "CALLS,HTTP_CALLS,ASYNC_CALLS", 5, 3, hits),
              3);
    ASSERT_STR_EQ(hits[2].name, "Epsilon");
    plugin_fixture_cleanup(&fx);
    PASS();
}

TEST(zova_plugin_trace_path_rejects_invalid_arguments) {
    plugin_fixture_t fx;
    ASSERT_EQ(plugin_fixture_create(&fx), 0);
    int64_t alpha = plugin_node_key(&fx, "proj.Alpha");
    plugin_hit_t hits[TZP_ROWS_MAX];
    ASSERT_EQ(plugin_trace(&fx, alpha, "both", "CALLS", 5, 20, hits), -1);
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "", 5, 20, hits), -1);
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "CALLS,", 5, 20, hits), -1);
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "CALLS", 5, 0, hits), -1);
    ASSERT_EQ(plugin_trace(&fx, 0, "outbound", "CALLS", 5, 20, hits), -1);
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q", 5, 20,
                           hits), -1);
    /* A failed call leaves the operation usable. */
    ASSERT_EQ(plugin_trace(&fx, alpha, "outbound", "CALLS", 5, 20, hits), 2);
    plugin_fixture_cleanup(&fx);
    PASS();
}

typedef struct {
    int64_t source;
    int64_t target;
    char type[TZP_TEXT_MAX];
    char local_name[TZP_TEXT_MAX];
    char properties[TZP_TEXT_MAX];
} plugin_edge_hit_t;

/* Returns the row count, or -1 when the statement fails. */
static int plugin_edges(const plugin_fixture_t *fx, int64_t node_key, const char *direction,
                        const char *edge_types, plugin_edge_hit_t *hits) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT source_key,target_key,edge_type,local_name,properties "
             "FROM zova_cbm_edges('%s',%lld,'%s','%s') ORDER BY edge_type,local_name",
             fx->graph_name, (long long)node_key, direction, edge_types);
    zova_statement *stmt = NULL;
    if (zova_database_prepare(&(zova_database_prepare_request){
            .db = fx->db, .sql = sql, .out_statement = &stmt}) != ZOVA_OK) return -1;
    int count = 0;
    while (count < TZP_ROWS_MAX) {
        zova_step_result step = 0;
        if (zova_statement_step(&(zova_statement_step_request){
                .statement = stmt, .out_result = &step}) != ZOVA_OK) {
            count = -1;
            break;
        }
        if (step != ZOVA_STEP_ROW) break;
        plugin_edge_hit_t *hit = &hits[count++];
        (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = 0, .out_value = &hit->source});
        (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = 1, .out_value = &hit->target});
        plugin_copy_text(stmt, 2, hit->type, sizeof(hit->type));
        plugin_copy_text(stmt, 3, hit->local_name, sizeof(hit->local_name));
        plugin_copy_text(stmt, 4, hit->properties, sizeof(hit->properties));
    }
    zova_statement_finalize(stmt);
    return count;
}

TEST(zova_plugin_edges_return_relationship_properties) {
    plugin_fixture_t fx;
    ASSERT_EQ(plugin_fixture_create(&fx), 0);
    int64_t alpha = plugin_node_key(&fx, "proj.Alpha");
    int64_t beta = plugin_node_key(&fx, "proj.Beta");
    int64_t delta = plugin_node_key(&fx, "proj.Delta");
    plugin_edge_hit_t hits[TZP_ROWS_MAX];

    /* Every type: both cbm edges of the Alpha->Beta graph edge, then Delta. */
    ASSERT_EQ(plugin_edges(&fx, alpha, "outbound", "", hits), 3);
    ASSERT_STR_EQ(hits[0].type, "CALLS");
    ASSERT_STR_EQ(hits[0].local_name, "Beta");
    ASSERT_STR_EQ(hits[0].properties, "{\"args\":\"x\"}");
    ASSERT_EQ(hits[0].source, alpha);
    ASSERT_EQ(hits[0].target, beta);
    ASSERT_STR_EQ(hits[1].local_name, "BetaAlias");
    ASSERT_STR_EQ(hits[1].properties, "{\"args\":\"y\"}");
    ASSERT_EQ(hits[1].target, beta);
    ASSERT_STR_EQ(hits[2].type, "HTTP_CALLS");
    ASSERT_EQ(hits[2].target, delta);

    ASSERT_EQ(plugin_edges(&fx, alpha, "outbound", "HTTP_CALLS", hits), 1);
    ASSERT_EQ(hits[0].target, delta);
    ASSERT_EQ(plugin_edges(&fx, alpha, "outbound", "HTTP_CALLS,ASYNC_CALLS", hits), 1);
    ASSERT_EQ(plugin_edges(&fx, alpha, "outbound", "NO_SUCH_TYPE", hits), 0);

    /* Inbound keeps source and target in edge direction. */
    ASSERT_EQ(plugin_edges(&fx, beta, "inbound", "CALLS", hits), 2);
    ASSERT_EQ(hits[0].source, alpha);
    ASSERT_EQ(hits[0].target, beta);
    ASSERT_STR_EQ(hits[0].properties, "{\"args\":\"x\"}");

    ASSERT_EQ(plugin_edges(&fx, alpha, "both", "", hits), -1);
    ASSERT_EQ(plugin_edges(&fx, alpha, "outbound", "CALLS,", hits), -1);
    ASSERT_EQ(plugin_edges(&fx, 0, "outbound", "", hits), -1);
    plugin_fixture_cleanup(&fx);
    PASS();
}

/* 300 edges with 6 KB of properties each: a 256-key payload batch is over the
 * 1 MiB read budget and has to be retried in halves. */
TEST(zova_plugin_edges_retry_payload_batches_over_budget) {
    enum { LEAVES = 300, NODES = LEAVES + 1, PROPS = 6000 };
    plugin_fixture_t fx;
    memset(&fx, 0, sizeof(fx));
    snprintf(fx.path, sizeof(fx.path), "%s/cbm-plugin-big-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)&fx);
    cbm_unlink(fx.path);
    CBMDumpNode *nodes = calloc(NODES, sizeof(*nodes));
    CBMDumpEdge *edges = calloc(LEAVES, sizeof(*edges));
    char (*text)[2][32] = calloc(NODES, sizeof(*text));
    char *properties = malloc(PROPS + 1);
    ASSERT_NOT_NULL(nodes);
    ASSERT_NOT_NULL(edges);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(properties);
    memset(properties, 'p', PROPS);
    memcpy(properties, "{\"a\":\"", 6);
    memcpy(properties + PROPS - 2, "\"}", 2);
    properties[PROPS] = '\0';
    for (int i = 0; i < NODES; i++) {
        snprintf(text[i][0], sizeof(text[i][0]), "B%05d", i);
        snprintf(text[i][1], sizeof(text[i][1]), "proj.B%05d", i);
        nodes[i] = (CBMDumpNode){.id = i + 1, .project = "proj", .label = "Function",
                                 .name = text[i][0], .qualified_name = text[i][1],
                                 .file_path = "big.c", .start_line = i + 1, .end_line = i + 1,
                                 .properties = "{}"};
    }
    for (int i = 0; i < LEAVES; i++) {
        edges[i] = (CBMDumpEdge){.id = i + 1, .project = "proj", .source_id = 1,
                                 .target_id = i + 2, .type = "CALLS", .properties = properties};
    }
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/plugin-trace-big", .project = "proj",
        .indexed_at = "2026-10-13T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
        .nodes = nodes, .node_count = NODES, .edges = edges, .edge_count = LEAVES,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(fx.path, &input, &published), 0);
    free(nodes);
    free(edges);
    free(text);
    free(properties);
    ASSERT_EQ(cbm_zova_workspace_graph_name(published.workspace_id, fx.graph_name,
                                            sizeof(fx.graph_name)), 0);
    ASSERT_EQ(plugin_open(fx.path, 1, &fx.db), ZOVA_OK);
    ASSERT_EQ(zova_database_extension_install(&(zova_database_extension_request){
                  .db = fx.db, .name = "cbm"}), ZOVA_OK);

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT count(*),count(DISTINCT target_key),sum(length(properties)=%d) "
             "FROM zova_cbm_edges('%s',%lld,'outbound','CALLS')",
             PROPS, fx.graph_name, (long long)plugin_node_key(&fx, "proj.B00000"));
    zova_statement *stmt = NULL;
    ASSERT_EQ(zova_database_prepare(&(zova_database_prepare_request){
                  .db = fx.db, .sql = sql, .out_statement = &stmt}), ZOVA_OK);
    zova_step_result step = 0;
    ASSERT_EQ(zova_statement_step(&(zova_statement_step_request){
                  .statement = stmt, .out_result = &step}), ZOVA_OK);
    ASSERT_EQ(step, ZOVA_STEP_ROW);
    for (int i = 0; i < 3; i++) {
        int64_t value = 0;
        (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = i, .out_value = &value});
        ASSERT_EQ(value, LEAVES);
    }
    zova_statement_finalize(stmt);
    plugin_fixture_cleanup(&fx);
    PASS();
}

/* 5000 callees exceed one adjacency page (4096 rows), one frontier batch (1024
 * keys) and one node-read batch (128 keys). */
TEST(zova_plugin_trace_path_pages_wide_fanout) {
    enum { LEAVES = 5000, NODES = LEAVES + 2 };
    plugin_fixture_t fx;
    memset(&fx, 0, sizeof(fx));
    snprintf(fx.path, sizeof(fx.path), "%s/cbm-plugin-wide-%d-%p.zova", cbm_tmpdir(),
             (int)getpid(), (void *)&fx);
    cbm_unlink(fx.path);
    CBMDumpNode *nodes = calloc(NODES, sizeof(*nodes));
    CBMDumpEdge *edges = calloc((size_t)LEAVES * 2, sizeof(*edges));
    char (*text)[2][32] = calloc(NODES, sizeof(*text));
    ASSERT_NOT_NULL(nodes);
    ASSERT_NOT_NULL(edges);
    ASSERT_NOT_NULL(text);
    /* Node 1 is the hub, node NODES the sink every leaf calls. */
    for (int i = 0; i < NODES; i++) {
        snprintf(text[i][0], sizeof(text[i][0]), "N%05d", i);
        snprintf(text[i][1], sizeof(text[i][1]), "proj.N%05d", i);
        nodes[i] = (CBMDumpNode){.id = i + 1, .project = "proj", .label = "Function",
                                 .name = text[i][0], .qualified_name = text[i][1],
                                 .file_path = "wide.c", .start_line = i + 1, .end_line = i + 1,
                                 .properties = "{}"};
    }
    for (int i = 0; i < LEAVES; i++) {
        edges[i * 2] = (CBMDumpEdge){.id = i * 2 + 1, .project = "proj", .source_id = 1,
                                     .target_id = i + 2, .type = "CALLS", .properties = "{}"};
        edges[i * 2 + 1] = (CBMDumpEdge){.id = i * 2 + 2, .project = "proj", .source_id = i + 2,
                                         .target_id = NODES, .type = "CALLS",
                                         .properties = "{}"};
    }
    cbm_zova_workspace_generation_input_t input = {
        .root_path = "/tmp/plugin-trace-wide", .project = "proj",
        .indexed_at = "2026-10-13T00:00:00Z",
        .model_fingerprint = CBM_ZOVA_MODEL_FINGERPRINT, .vector_dimensions = 2,
        .nodes = nodes, .node_count = NODES, .edges = edges, .edge_count = LEAVES * 2,
    };
    cbm_zova_workspace_generation_result_t published = {0};
    ASSERT_EQ(cbm_zova_user_database_publish_workspace(fx.path, &input, &published), 0);
    free(nodes);
    free(edges);
    free(text);
    ASSERT_EQ(cbm_zova_workspace_graph_name(published.workspace_id, fx.graph_name,
                                            sizeof(fx.graph_name)), 0);
    ASSERT_EQ(plugin_open(fx.path, 1, &fx.db), ZOVA_OK);
    ASSERT_EQ(zova_database_extension_install(&(zova_database_extension_request){
                  .db = fx.db, .name = "cbm"}), ZOVA_OK);

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT count(*),count(DISTINCT node_key),sum(hop=1),sum(hop=2),"
             "max(CASE WHEN hop=2 THEN name END) "
             "FROM zova_cbm_trace_path('%s',%lld,'outbound','CALLS',5,100000)",
             fx.graph_name, (long long)plugin_node_key(&fx, "proj.N00000"));
    zova_statement *stmt = NULL;
    ASSERT_EQ(zova_database_prepare(&(zova_database_prepare_request){
                  .db = fx.db, .sql = sql, .out_statement = &stmt}), ZOVA_OK);
    zova_step_result step = 0;
    ASSERT_EQ(zova_statement_step(&(zova_statement_step_request){
                  .statement = stmt, .out_result = &step}), ZOVA_OK);
    ASSERT_EQ(step, ZOVA_STEP_ROW);
    int64_t counts[4] = {0};
    for (int i = 0; i < 4; i++) {
        (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = i, .out_value = &counts[i]});
    }
    char sink[TZP_TEXT_MAX];
    plugin_copy_text(stmt, 4, sink, sizeof(sink));
    zova_statement_finalize(stmt);
    ASSERT_EQ(counts[0], LEAVES + 1);
    ASSERT_EQ(counts[1], LEAVES + 1);
    ASSERT_EQ(counts[2], LEAVES);
    ASSERT_EQ(counts[3], 1);
    ASSERT_STR_EQ(sink, "N05001");

    /* 5000 edges: paged adjacency and batched payload reads. */
    snprintf(sql, sizeof(sql),
             "SELECT count(*),count(DISTINCT target_key),sum(properties='{}'),"
             "sum(edge_type='CALLS') FROM zova_cbm_edges('%s',%lld,'outbound','')",
             fx.graph_name, (long long)plugin_node_key(&fx, "proj.N00000"));
    stmt = NULL;
    ASSERT_EQ(zova_database_prepare(&(zova_database_prepare_request){
                  .db = fx.db, .sql = sql, .out_statement = &stmt}), ZOVA_OK);
    ASSERT_EQ(zova_statement_step(&(zova_statement_step_request){
                  .statement = stmt, .out_result = &step}), ZOVA_OK);
    ASSERT_EQ(step, ZOVA_STEP_ROW);
    for (int i = 0; i < 4; i++) {
        (void)zova_statement_column_int64(&(zova_statement_column_int64_request){
            .statement = stmt, .index = i, .out_value = &counts[i]});
        ASSERT_EQ(counts[i], LEAVES);
    }
    zova_statement_finalize(stmt);

    /* max_results keeps the first rows of the full ordering. */
    plugin_hit_t hits[TZP_ROWS_MAX];
    ASSERT_EQ(plugin_trace(&fx, plugin_node_key(&fx, "proj.N00000"), "outbound", "CALLS", 5, 2,
                           hits), 2);
    ASSERT_STR_EQ(hits[0].name, "N00001");
    ASSERT_STR_EQ(hits[1].name, "N00002");
    plugin_fixture_cleanup(&fx);
    PASS();
}

enum { TZP_CYPHER_TEXT = 4096, TZP_CYPHER_ROWS = 64 };

/* A Cypher result as text: line 0 is the columns, the rest are the rows sorted,
 * cells separated by '|'. An error is the single line "error". */
typedef struct {
    char lines[TZP_CYPHER_ROWS][TZP_TEXT_MAX * 2];
    int count;
} plugin_cypher_text_t;

static int plugin_line_compare(const void *a, const void *b) {
    return strcmp(a, b);
}

static void plugin_cypher_sort(plugin_cypher_text_t *text) {
    if (text->count > 2) {
        qsort(text->lines[1], (size_t)text->count - 1, sizeof(text->lines[0]),
              plugin_line_compare);
    }
}

static void plugin_cypher_line(plugin_cypher_text_t *text, const char *const *cells, int count) {
    char *line = text->lines[text->count++];
    size_t used = 0;
    line[0] = '\0';
    for (int i = 0; i < count && used < sizeof(text->lines[0]); i++) {
        used += (size_t)snprintf(line + used, sizeof(text->lines[0]) - used, "%s%s", i ? "|" : "",
                                 cells[i] ? cells[i] : "<null>");
    }
}

static int plugin_cypher_host(const plugin_fixture_t *fx, const char *query,
                              plugin_cypher_text_t *text) {
    memset(text, 0, sizeof(*text));
    cbm_store_t *store = cbm_store_open_zova_workspace_query(
        fx->path, fx->graph_name + strlen("cbm_graph_"));
    if (!store) return -1;
    cbm_cypher_result_t result = {0};
    if (cbm_cypher_execute(store, query, "proj", 0, &result) != 0) {
        snprintf(text->lines[text->count++], sizeof(text->lines[0]), "error");
    } else if (result.row_count + 1 > TZP_CYPHER_ROWS) {
        text->count = -1;
    } else {
        plugin_cypher_line(text, result.columns, result.col_count);
        for (int i = 0; i < result.row_count; i++) {
            plugin_cypher_line(text, result.rows[i], result.col_count);
        }
        plugin_cypher_sort(text);
    }
    cbm_cypher_result_free(&result);
    cbm_store_close(store);
    return text->count;
}

/* Returns the line count, or -1 when the statement fails. message receives the
 * cells of an 'error' row. */
static int plugin_cypher(const plugin_fixture_t *fx, const char *workspace_id, const char *query,
                         plugin_cypher_text_t *text, char *message, size_t message_size) {
    memset(text, 0, sizeof(*text));
    if (message_size) message[0] = '\0';
    zova_statement *stmt = NULL;
    if (zova_database_prepare(&(zova_database_prepare_request){
            .db = fx->db, .sql = "SELECT kind,cells FROM zova_cbm_cypher(?1,?2,0)",
            .out_statement = &stmt}) != ZOVA_OK) return -1;
    int ok = zova_statement_bind_text(&(zova_statement_bind_text_request){
                 .statement = stmt, .index = 1, .data = (const uint8_t *)workspace_id,
                 .len = strlen(workspace_id)}) == ZOVA_OK &&
             zova_statement_bind_text(&(zova_statement_bind_text_request){
                 .statement = stmt, .index = 2, .data = (const uint8_t *)query,
                 .len = strlen(query)}) == ZOVA_OK;
    while (ok && text->count < TZP_CYPHER_ROWS) {
        zova_step_result step = 0;
        ok = zova_statement_step(&(zova_statement_step_request){
                 .statement = stmt, .out_result = &step}) == ZOVA_OK;
        if (!ok || step != ZOVA_STEP_ROW) break;
        char kind[TZP_TEXT_MAX];
        char cells[TZP_CYPHER_TEXT];
        plugin_copy_text(stmt, 0, kind, sizeof(kind));
        plugin_copy_text(stmt, 1, cells, sizeof(cells));
        if (strcmp(kind, "error") == 0) {
            snprintf(text->lines[text->count++], sizeof(text->lines[0]), "error");
            snprintf(message, message_size, "%s", cells);
            continue;
        }
        if (strcmp(kind, "warning") == 0) continue;
        yyjson_doc *doc = yyjson_read(cells, strlen(cells), 0);
        yyjson_val *root = yyjson_doc_get_root(doc);
        const char *values[TZP_ROWS_MAX];
        int count = 0;
        size_t index;
        size_t max;
        yyjson_val *value;
        ok = yyjson_is_arr(root) && yyjson_arr_size(root) <= TZP_ROWS_MAX;
        if (ok) {
            yyjson_arr_foreach(root, index, max, value) {
                values[count++] = yyjson_get_str(value);
            }
            plugin_cypher_line(text, values, count);
        }
        yyjson_doc_free(doc);
    }
    zova_statement_finalize(stmt);
    if (!ok) return -1;
    plugin_cypher_sort(text);
    return text->count;
}

/* The plugin runs the host's Cypher engine over plugin services, so both must
 * answer alike. Host results are taken first: the host cannot open the
 * database once the plugin is installed. */
TEST(zova_plugin_cypher_matches_host_cypher) {
    static const char *const queries[] = {
        "MATCH (n:Function) RETURN n.name, n.qualified_name, n.file_path",
        "MATCH (n) WHERE n.name = 'Beta' RETURN n.name, n.label, n.start_line, n.k",
        "MATCH (a)-[r:CALLS]->(b) RETURN a.name, r.args, b.name",
        "MATCH (a)-[r]->(b) WHERE a.name = 'Alpha' RETURN b.name, type(r)",
        "MATCH (a)<-[:CALLS]-(b) WHERE a.name = 'Beta' RETURN b.name",
        "MATCH (a)-[:HTTP_CALLS]->(b)-[:ASYNC_CALLS]->(c) RETURN a.name, b.name, c.name",
        "MATCH (a:Function)-[:CALLS*1..3]->(b) WHERE a.name = 'Alpha' RETURN b.name",
        "MATCH (a)-[r:CALLS]->(b) WHERE r.args = 'y' RETURN a.name, b.name",
        "MATCH (n:Function) RETURN count(n)",
        "MATCH (n:NoSuchLabel) RETURN n.name",
        "MATCH (",
    };
    enum { QUERIES = sizeof(queries) / sizeof(queries[0]) };
    static plugin_cypher_text_t host[QUERIES];
    static plugin_cypher_text_t plugin;
    plugin_fixture_t fx;
    char message[TZP_CYPHER_TEXT];
    ASSERT_EQ(plugin_fixture_publish(&fx), 0);
    for (int i = 0; i < QUERIES; i++) ASSERT_GT(plugin_cypher_host(&fx, queries[i], &host[i]), 0);
    /* The relationship-property query has rows to compare. */
    ASSERT_EQ(host[2].count, 5);
    ASSERT_STR_EQ(host[2].lines[1], "Alpha|x|Beta");
    ASSERT_STR_EQ(host[2].lines[2], "Alpha|y|Beta");
    ASSERT_STR_EQ(host[QUERIES - 1].lines[0], "error");

    ASSERT_EQ(plugin_fixture_install(&fx), 0);
    const char *workspace_id = fx.graph_name + strlen("cbm_graph_");
    for (int i = 0; i < QUERIES; i++) {
        ASSERT_EQ(plugin_cypher(&fx, workspace_id, queries[i], &plugin, message, sizeof(message)),
                  host[i].count);
        for (int line = 0; line < host[i].count; line++) {
            ASSERT_STR_EQ(plugin.lines[line], host[i].lines[line]);
        }
    }

    ASSERT_EQ(plugin_cypher(&fx, "no_such_workspace", queries[0], &plugin, message,
                            sizeof(message)), 1);
    ASSERT_STR_EQ(plugin.lines[0], "error");
    ASSERT_STR_EQ(message, "workspace not found");
    ASSERT_EQ(plugin_cypher(&fx, "not-a-workspace-id", queries[0], &plugin, message,
                            sizeof(message)), -1);
    ASSERT_EQ(plugin_cypher(&fx, workspace_id, "", &plugin, message, sizeof(message)), -1);
    plugin_fixture_cleanup(&fx);
    PASS();
}

/* Why the plugin stays out of user databases until every open path links it. */
TEST(zova_plugin_installed_database_requires_linked_plugin) {
    plugin_fixture_t fx;
    ASSERT_EQ(plugin_fixture_create(&fx), 0);
    ASSERT_EQ(zova_database_close(fx.db), ZOVA_OK);
    fx.db = NULL;

    zova_database *db = NULL;
    ASSERT_NEQ(plugin_open(fx.path, 0, &db), ZOVA_OK);
    ASSERT_NULL(db);

    ASSERT_EQ(plugin_open(fx.path, 1, &fx.db), ZOVA_OK);
    plugin_hit_t hits[TZP_ROWS_MAX];
    ASSERT_EQ(plugin_trace(&fx, plugin_node_key(&fx, "proj.Alpha"), "outbound", "CALLS", 5, 20,
                           hits), 2);
    plugin_fixture_cleanup(&fx);
    PASS();
}

#endif

SUITE(zova_plugin) {
#if !CBM_WITH_ZOVA
    RUN_TEST(zova_plugin_disabled);
#else
    RUN_TEST(zova_plugin_trace_path_follows_one_edge_type);
    RUN_TEST(zova_plugin_trace_path_follows_edge_type_set);
    RUN_TEST(zova_plugin_trace_path_rejects_invalid_arguments);
    RUN_TEST(zova_plugin_edges_return_relationship_properties);
    RUN_TEST(zova_plugin_edges_retry_payload_batches_over_budget);
    RUN_TEST(zova_plugin_trace_path_pages_wide_fanout);
    RUN_TEST(zova_plugin_cypher_matches_host_cypher);
    RUN_TEST(zova_plugin_installed_database_requires_linked_plugin);
#endif
}
