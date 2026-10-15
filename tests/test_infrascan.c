#include "test_framework.h"
#include "graph_buffer/graph_buffer.h"
#include "pipeline/pipeline_internal.h"

#include <stdbool.h>
#include <stdint.h>

bool cbm_service_pattern_is_http_route_literal(const char *literal, const char *callee_name);

static int has_edge(cbm_gbuf_t *gb, int64_t source_id, int64_t target_id, const char *type) {
    const cbm_gbuf_edge_t **edges = NULL;
    int count = 0;
    cbm_gbuf_find_edges_by_source_type(gb, source_id, type, &edges, &count);
    for (int i = 0; i < count; i++) {
        if (edges[i]->target_id == target_id) {
            return 1;
        }
    }
    return 0;
}

static int has_data_flow(cbm_gbuf_t *gb, int64_t source_id, int64_t target_id) {
    return has_edge(gb, source_id, target_id, "DATA_FLOWS");
}

static int count_handles_into(cbm_gbuf_t *gb, int64_t target_id) {
    const cbm_gbuf_edge_t **edges = NULL;
    int count = 0;
    cbm_gbuf_find_edges_by_target_type(gb, target_id, "HANDLES", &edges, &count);
    return count;
}

static int64_t add_infra_route(cbm_gbuf_t *gb, const char *url) {
    char qn[512];
    snprintf(qn, sizeof(qn), "__route__infra__%s", url);
    return cbm_gbuf_upsert_node(gb, "Route", url, qn, "infra/config.yaml", 0, 0,
                                "{\"source\":\"infra\"}");
}

/* A handler Route with one HANDLES edge from a new Function in the same file. */
static int64_t add_handled_route(cbm_gbuf_t *gb, const char *method, const char *path,
                                 const char *file_path, const char *handler_name,
                                 int64_t *out_handler) {
    char qn[256];
    snprintf(qn, sizeof(qn), "__route__%s__%s", method, path);
    int64_t route = cbm_gbuf_upsert_node(gb, "Route", path, qn, file_path, 0, 0, "{}");
    char handler_qn[256];
    snprintf(handler_qn, sizeof(handler_qn), "test.%s", handler_name);
    int64_t handler = cbm_gbuf_upsert_node(gb, "Function", handler_name, handler_qn,
                                           file_path[0] ? file_path : "handlers.py", 1, 3, "{}");
    cbm_gbuf_insert_edge(gb, handler, route, "HANDLES", "{}");
    if (out_handler) {
        *out_handler = handler;
    }
    return route;
}

TEST(infrascan_http_route_literal_guard_rejects_filesystem_paths) {
    ASSERT_FALSE(cbm_service_pattern_is_http_route_literal("/etc/crio/crio.conf", "requests.get"));
    ASSERT_FALSE(
        cbm_service_pattern_is_http_route_literal("/root/.aws/credentials", "requests.get"));
    ASSERT_FALSE(cbm_service_pattern_is_http_route_literal("/var/run/app.json", "requests.get"));
    ASSERT_FALSE(cbm_service_pattern_is_http_route_literal("/locations/", "str.split"));
    ASSERT_FALSE(cbm_service_pattern_is_http_route_literal("/api", "os.path.join"));
    ASSERT_FALSE(cbm_service_pattern_is_http_route_literal("/html/g", "template.replace"));
    ASSERT_FALSE(cbm_service_pattern_is_http_route_literal(NULL, "requests.get"));
    ASSERT_FALSE(cbm_service_pattern_is_http_route_literal("", "requests.get"));
    ASSERT_TRUE(cbm_service_pattern_is_http_route_literal("/api/orders", "requests.get"));
    ASSERT_TRUE(cbm_service_pattern_is_http_route_literal("https://orders.example/api/orders",
                                                          "requests.get"));
    PASS();
}

TEST(infrascan_route_nodes_skip_bad_http_url_paths) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test", "/tmp/cbm_infrascan_route_guard");
    ASSERT_NOT_NULL(gb);
    int64_t caller =
        cbm_gbuf_upsert_node(gb, "Function", "client", "test.client", "client.py", 1, 3, "{}");
    int64_t fs_callee =
        cbm_gbuf_upsert_node(gb, "Function", "requests.get", "requests.get", "", 0, 0, "{}");
    int64_t split_callee =
        cbm_gbuf_upsert_node(gb, "Function", "str.split", "str.split", "", 0, 0, "{}");
    int64_t empty_callee =
        cbm_gbuf_upsert_node(gb, "Function", "requests.post", "requests.post", "", 0, 0, "{}");
    ASSERT_GT(caller, 0);
    ASSERT_GT(fs_callee, 0);
    ASSERT_GT(split_callee, 0);
    ASSERT_GT(empty_callee, 0);

    cbm_gbuf_insert_edge(gb, caller, fs_callee, "HTTP_CALLS",
                         "{\"callee\":\"requests.get\",\"url_path\":\"/etc/crio/crio.conf\","
                         "\"method\":\"GET\"}");
    cbm_gbuf_insert_edge(gb, caller, split_callee, "HTTP_CALLS",
                         "{\"callee\":\"str.split\",\"url_path\":\"/locations/\","
                         "\"method\":\"ANY\"}");
    cbm_gbuf_insert_edge(gb, caller, empty_callee, "HTTP_CALLS",
                         "{\"callee\":\"requests.get\",\"method\":\"GET\"}");

    cbm_pipeline_create_route_nodes(gb);

    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "__route__GET__/etc/crio/crio.conf"));
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "__route__ANY__/locations/"));
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "__route__GET__"));

    cbm_gbuf_free(gb);
    PASS();
}

TEST(infrascan_http_calls_join_matching_handler_route) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test", "/tmp/cbm_infrascan_route_join");
    ASSERT_NOT_NULL(gb);

    int64_t route = cbm_gbuf_upsert_node(gb, "Route", "/api/orders", "__route__GET__/api/orders",
                                         "", 0, 0, "{\"method\":\"GET\"}");
    int64_t handler = cbm_gbuf_upsert_node(gb, "Function", "get_orders", "test.get_orders",
                                           "server.py", 1, 3, "{}");
    int64_t client =
        cbm_gbuf_upsert_node(gb, "Function", "client", "test.client", "client.py", 1, 3, "{}");
    int64_t bad_route =
        cbm_gbuf_upsert_node(gb, "Route", "/etc/crio/crio.conf",
                             "__route__GET__/etc/crio/crio.conf", "", 0, 0, "{\"method\":\"GET\"}");
    int64_t bad_handler = cbm_gbuf_upsert_node(gb, "Function", "bad_handler", "test.bad_handler",
                                               "server.py", 5, 7, "{}");
    int64_t bad_client = cbm_gbuf_upsert_node(gb, "Function", "bad_client", "test.bad_client",
                                              "client.py", 5, 7, "{}");
    ASSERT_GT(route, 0);
    ASSERT_GT(handler, 0);
    ASSERT_GT(client, 0);
    ASSERT_GT(bad_route, 0);
    ASSERT_GT(bad_handler, 0);
    ASSERT_GT(bad_client, 0);

    cbm_gbuf_insert_edge(gb, handler, route, "HANDLES", "{\"handler\":\"test.get_orders\"}");
    cbm_gbuf_insert_edge(gb, client, route, "HTTP_CALLS",
                         "{\"callee\":\"requests.get\",\"url_path\":\"/api/orders\","
                         "\"method\":\"GET\"}");
    cbm_gbuf_insert_edge(gb, bad_handler, bad_route, "HANDLES",
                         "{\"handler\":\"test.bad_handler\"}");
    cbm_gbuf_insert_edge(gb, bad_client, bad_route, "HTTP_CALLS",
                         "{\"callee\":\"requests.get\",\"url_path\":\"/etc/crio/crio.conf\","
                         "\"method\":\"GET\"}");

    cbm_pipeline_create_route_nodes(gb);

    ASSERT_TRUE(has_data_flow(gb, client, handler));
    ASSERT_FALSE(has_data_flow(gb, bad_client, bad_handler));

    cbm_gbuf_free(gb);
    PASS();
}

/* Only config keys that name an endpoint the service is called at mint infra
 * Routes; data-file URLs (avatars, profile links, images) do not. */
TEST(infrascan_infra_route_key_allowlist) {
    ASSERT_TRUE(cbm_pipeline_infra_route_key_allowed("subscriptions.config.push_endpoint"));
    ASSERT_TRUE(cbm_pipeline_infra_route_key_allowed("pushConfig.pushEndpoint"));
    ASSERT_TRUE(cbm_pipeline_infra_route_key_allowed("httpTarget.uri"));
    ASSERT_TRUE(cbm_pipeline_infra_route_key_allowed("body.operational_info.post_url"));
    ASSERT_TRUE(cbm_pipeline_infra_route_key_allowed("hooks.webhook_url"));
    ASSERT_TRUE(cbm_pipeline_infra_route_key_allowed("callback"));
    ASSERT_FALSE(cbm_pipeline_infra_route_key_allowed("url"));
    ASSERT_FALSE(cbm_pipeline_infra_route_key_allowed("avatarUrl"));
    ASSERT_FALSE(cbm_pipeline_infra_route_key_allowed("link"));
    ASSERT_FALSE(cbm_pipeline_infra_route_key_allowed("img"));
    ASSERT_FALSE(cbm_pipeline_infra_route_key_allowed("repo"));
    ASSERT_FALSE(cbm_pipeline_infra_route_key_allowed("registries.terraform-registry.url"));
    ASSERT_FALSE(cbm_pipeline_infra_route_key_allowed("auth.jwks_endpoint"));
    ASSERT_FALSE(cbm_pipeline_infra_route_key_allowed(NULL));
    PASS();
}

/* A URL without a path names no endpoint: it matches neither the "/" prefix
 * Route nor another service's root handler. */
TEST(infrascan_infra_url_without_path_matches_nothing) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test", "/tmp/cbm_infrascan_no_path");
    ASSERT_NOT_NULL(gb);
    add_handled_route(gb, "ANY", "/", "", "any_root", NULL);
    add_handled_route(gb, "GET", "/", "services/billing/main.py", "billing_root", NULL);
    int64_t profile = add_infra_route(gb, "https://github.com");
    int64_t orders = add_infra_route(gb, "https://orders.example.com");

    cbm_pipeline_create_route_nodes(gb);

    ASSERT_EQ(count_handles_into(gb, profile), 0);
    ASSERT_EQ(count_handles_into(gb, orders), 0);
    cbm_gbuf_free(gb);
    PASS();
}

/* A root handler only stands for its own service's root URL. */
TEST(infrascan_infra_root_handler_needs_same_service_root_url) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test", "/tmp/cbm_infrascan_root");
    ASSERT_NOT_NULL(gb);
    int64_t root_handler = 0;
    add_handled_route(gb, "GET", "/", "services/orders/main.py", "orders_root", &root_handler);
    int64_t root_url = add_infra_route(gb, "https://orders.example.com/");
    int64_t other_path = add_infra_route(gb, "https://orders.example.com/missing");

    cbm_pipeline_create_route_nodes(gb);

    ASSERT_TRUE(has_edge(gb, root_handler, root_url, "HANDLES"));
    ASSERT_EQ(count_handles_into(gb, other_path), 0);
    cbm_gbuf_free(gb);
    PASS();
}

/* Paths match on whole segments: /api/v1 covers /api/v1/items and
 * /api/v1?x=1 but not /api/v10. */
TEST(infrascan_infra_prefix_match_is_segment_aware) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test", "/tmp/cbm_infrascan_segments");
    ASSERT_NOT_NULL(gb);
    int64_t v1_handler = 0;
    add_handled_route(gb, "ANY", "/api/v1", "", "v1_router", &v1_handler);
    int64_t items = add_infra_route(gb, "https://shop.example.com/api/v1/items");
    int64_t query = add_infra_route(gb, "https://shop.example.com/api/v1?x=1");
    int64_t v10 = add_infra_route(gb, "https://shop.example.com/api/v10/items");

    cbm_pipeline_create_route_nodes(gb);

    ASSERT_TRUE(has_edge(gb, v1_handler, items, "HANDLES"));
    ASSERT_TRUE(has_edge(gb, v1_handler, query, "HANDLES"));
    ASSERT_EQ(count_handles_into(gb, v10), 0);
    cbm_gbuf_free(gb);
    PASS();
}

/* The most specific handler Route wins whatever order the Routes were added
 * in (parallel extraction changes that order between runs). */
TEST(infrascan_infra_match_prefers_longest_path_in_any_order) {
    for (int order = 0; order < 2; order++) {
        cbm_gbuf_t *gb = cbm_gbuf_new("test", "/tmp/cbm_infrascan_order");
        ASSERT_NOT_NULL(gb);
        int64_t api_handler = 0;
        int64_t v1_handler = 0;
        if (order == 0) {
            add_handled_route(gb, "ANY", "/api", "", "api_router", &api_handler);
            add_handled_route(gb, "ANY", "/api/v1", "", "v1_router", &v1_handler);
        } else {
            add_handled_route(gb, "ANY", "/api/v1", "", "v1_router", &v1_handler);
            add_handled_route(gb, "ANY", "/api", "", "api_router", &api_handler);
        }
        int64_t infra = add_infra_route(gb, "https://shop.example.com/api/v1/items");

        cbm_pipeline_create_route_nodes(gb);

        ASSERT_TRUE(has_edge(gb, v1_handler, infra, "HANDLES"));
        ASSERT_FALSE(has_edge(gb, api_handler, infra, "HANDLES"));
        cbm_gbuf_free(gb);
    }
    PASS();
}

/* Every file that registers a prefix Route bridges its own directory's route
 * handlers; the root prefix "/" has nothing to scope by and bridges none. */
TEST(infrascan_prefix_bridge_uses_every_registrar) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test", "/tmp/cbm_infrascan_bridge");
    ASSERT_NOT_NULL(gb);
    int64_t prefix =
        cbm_gbuf_upsert_node(gb, "Route", "/api", "__route__ANY__/api", "", 0, 0, "{}");
    int64_t root = cbm_gbuf_upsert_node(gb, "Route", "/", "__route__ANY__/", "", 0, 0, "{}");
    int64_t reg_b =
        cbm_gbuf_upsert_node(gb, "Function", "setup_b", "b.setup", "svc_b/app.py", 1, 3, "{}");
    int64_t reg_a =
        cbm_gbuf_upsert_node(gb, "Function", "setup_a", "a.setup", "svc_a/app.py", 1, 3, "{}");
    int64_t items_a = cbm_gbuf_upsert_node(gb, "Function", "items_a", "a.items",
                                           "svc_a/api/routes.py", 1, 3,
                                           "{\"route_path\":\"/items\"}");
    int64_t items_b = cbm_gbuf_upsert_node(gb, "Function", "items_b", "b.items",
                                           "svc_b/api/routes.py", 1, 3,
                                           "{\"route_path\":\"/items\"}");
    cbm_gbuf_insert_edge(gb, reg_b, prefix, "CALLS", "{}");
    cbm_gbuf_insert_edge(gb, reg_a, prefix, "CALLS", "{}");
    cbm_gbuf_insert_edge(gb, reg_a, root, "CALLS", "{}");

    cbm_pipeline_create_route_nodes(gb);

    ASSERT_TRUE(has_edge(gb, items_a, prefix, "HANDLES"));
    ASSERT_TRUE(has_edge(gb, items_b, prefix, "HANDLES"));
    ASSERT_FALSE(has_edge(gb, items_a, root, "HANDLES"));
    cbm_gbuf_free(gb);
    PASS();
}

SUITE(infrascan) {
    RUN_TEST(infrascan_http_route_literal_guard_rejects_filesystem_paths);
    RUN_TEST(infrascan_route_nodes_skip_bad_http_url_paths);
    RUN_TEST(infrascan_http_calls_join_matching_handler_route);
    RUN_TEST(infrascan_infra_route_key_allowlist);
    RUN_TEST(infrascan_infra_url_without_path_matches_nothing);
    RUN_TEST(infrascan_infra_root_handler_needs_same_service_root_url);
    RUN_TEST(infrascan_infra_prefix_match_is_segment_aware);
    RUN_TEST(infrascan_infra_match_prefers_longest_path_in_any_order);
    RUN_TEST(infrascan_prefix_bridge_uses_every_registrar);
}
