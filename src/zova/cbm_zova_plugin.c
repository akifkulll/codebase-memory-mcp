#include "zova/cbm_zova_plugin.h"

#include <stddef.h>

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

#if !CBM_WITH_ZOVA

const struct zova_plugin_descriptor_v1 *cbm_zova_plugin_descriptor(void) {
    return NULL;
}

#else

#include "cypher/cypher.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_edge_payload.h"
#include "zova_plugin.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson/yyjson.h>

enum {
    PLUGIN_EDGE_TYPES_MAX = 16,
    PLUGIN_NAME_MAX = 255,
    PLUGIN_FRONTIER_BATCH = 1024,
    PLUGIN_HYDRATE_BATCH = 128,
    PLUGIN_PAGE_ROWS = 4096,
    PLUGIN_PAGE_BYTES = 1024 * 1024,
    PLUGIN_MAX_RESULTS = 100000,
    PLUGIN_TEXT_COLUMNS = 5,
    PLUGIN_NODE_COLUMNS = 8,
    PLUGIN_TRACE_ARGUMENTS = 6,
    PLUGIN_TRACE_COLUMNS = 9,
    PLUGIN_NEIGHBOR_COLUMNS = 6,
    PLUGIN_PAYLOAD_BATCH = 256,
    PLUGIN_PAYLOAD_COLUMNS = 4,
    PLUGIN_EDGES_ARGUMENTS = 4,
    PLUGIN_EDGES_COLUMNS = 6,
    PLUGIN_SCAN_PAGE = 2048,
    PLUGIN_CYPHER_ARGUMENTS = 3,
    PLUGIN_CYPHER_COLUMNS = 2,
};

#define PLUGIN_NODE_SELECT                                                              \
    "SELECT n.zova_node_key,n.label,n.name,n.qualified_name,f.file_path,n.start_line," \
    "n.end_line,n.properties FROM cbm_nodes_v1 n JOIN cbm_files_v1 f "                 \
    "ON f.file_key=n.file_key "

/* The host services one hook or operation callback may use. */
typedef struct {
    void *connection;
    const zova_plugin_data_service_v1 *data;
    const zova_plugin_query_service_v1 *query;
} plugin_session_t;

/* One reached node. text holds label, name, qualified_name, file_path and
 * properties back to back. */
typedef struct {
    int64_t key;
    int64_t start_line;
    int64_t end_line;
    int32_t hop;
    char *text;
    uint64_t off[PLUGIN_TEXT_COLUMNS];
    uint64_t len[PLUGIN_TEXT_COLUMNS];
} plugin_row_t;

enum { ROW_LABEL, ROW_NAME, ROW_QN, ROW_FILE, ROW_PROPERTIES };

typedef struct {
    plugin_row_t *rows;
    size_t count;
    size_t cap;
    size_t next;
    /* Visited node keys in discovery order; a level is a contiguous slice. */
    int64_t *keys;
    size_t key_count;
    size_t key_cap;
    /* Open-addressing set over keys (0 = empty; node keys are positive). */
    int64_t *set;
    size_t set_cap;
    int32_t level;
    uint64_t last_ordinal;
} plugin_trace_t;

static int32_t plugin_service(const zova_plugin_host_v1 *host, void *connection, uint32_t id,
                              uint32_t size, const void **out) {
    *out = NULL;
    if (!host || host->struct_size < sizeof(zova_plugin_service_host_v1)) {
        return ZOVA_PLUGIN_UNSUPPORTED;
    }
    return ((const zova_plugin_service_host_v1 *)host)->get_service(connection, id, 1, size, out);
}

static int32_t plugin_session_open(const zova_plugin_host_v1 *host, void *connection,
                                   plugin_session_t *session) {
    const void *data = NULL;
    const void *query = NULL;
    int32_t status = plugin_service(host, connection, ZOVA_PLUGIN_SERVICE_DATA,
                                    sizeof(zova_plugin_data_service_v1), &data);
    if (status == ZOVA_PLUGIN_OK) {
        status = plugin_service(host, connection, ZOVA_PLUGIN_SERVICE_QUERY,
                                sizeof(zova_plugin_query_service_v1), &query);
    }
    *session = (plugin_session_t){connection, data, query};
    return status;
}

static void plugin_trace_free(plugin_trace_t *trace) {
    if (!trace) return;
    for (size_t i = 0; i < trace->count; i++) free(trace->rows[i].text);
    free(trace->rows);
    free(trace->keys);
    free(trace->set);
    free(trace);
}

static size_t plugin_set_slot(const int64_t *set, size_t cap, int64_t key) {
    size_t slot = (size_t)((uint64_t)key * UINT64_C(0x9E3779B97F4A7C15)) & (cap - 1);
    while (set[slot] != 0 && set[slot] != key) slot = (slot + 1) & (cap - 1);
    return slot;
}

/* Returns 1 when key was added, 0 when already present, -1 on allocation failure. */
static int plugin_visit(plugin_trace_t *trace, int64_t key) {
    if ((trace->key_count + 1) * 2 > trace->set_cap) {
        size_t cap = trace->set_cap ? trace->set_cap * 2 : 1024;
        int64_t *set = calloc(cap, sizeof(*set));
        if (!set) return -1;
        for (size_t i = 0; i < trace->key_count; i++) {
            set[plugin_set_slot(set, cap, trace->keys[i])] = trace->keys[i];
        }
        free(trace->set);
        trace->set = set;
        trace->set_cap = cap;
    }
    size_t slot = plugin_set_slot(trace->set, trace->set_cap, key);
    if (trace->set[slot] == key) return 0;
    if (trace->key_count == trace->key_cap) {
        size_t cap = trace->key_cap ? trace->key_cap * 2 : 1024;
        int64_t *keys = realloc(trace->keys, cap * sizeof(*keys));
        if (!keys) return -1;
        trace->keys = keys;
        trace->key_cap = cap;
    }
    trace->set[slot] = key;
    trace->keys[trace->key_count++] = key;
    return 1;
}

/* NEIGHBORS_KEYED row: ordinal, input_key, edge_key, neighbor_key, edge_type, order. */
static int32_t ZOVA_PLUGIN_CALL plugin_neighbor_row(void *user_data,
                                                    const zova_plugin_value_v1 *values,
                                                    uint64_t count) {
    plugin_trace_t *trace = user_data;
    if (count != PLUGIN_NEIGHBOR_COLUMNS || values[0].kind != ZOVA_PLUGIN_VALUE_INTEGER ||
        values[0].integer < 0 || values[3].kind != ZOVA_PLUGIN_VALUE_INTEGER ||
        values[3].integer <= 0) {
        return ZOVA_PLUGIN_ERROR;
    }
    trace->last_ordinal = (uint64_t)values[0].integer;
    return plugin_visit(trace, values[3].integer) < 0 ? ZOVA_PLUGIN_OUT_OF_MEMORY : ZOVA_PLUGIN_OK;
}

/* Reads every matching neighbor of keys[0..count), following the keyed cursor. */
static int32_t plugin_expand(const plugin_session_t *session, plugin_trace_t *trace,
                             zova_plugin_data_request_v1 *request, size_t first, size_t count) {
    zova_plugin_cursor_v1 after = {0};
    while (count > 0) {
        zova_plugin_data_page_v1 page = {0};
        /* The row callback may grow trace->keys; re-read the pointer per page. */
        request->keys = trace->keys + first;
        request->key_count = count;
        request->after = after;
        trace->last_ordinal = 0;
        int32_t status = session->data->read(session->connection, request, &page);
        if (status != ZOVA_PLUGIN_OK) return status;
        if (!page.has_more) break;
        if (page.rows == 0 || trace->last_ordinal >= count) return ZOVA_PLUGIN_ERROR;
        first += (size_t)trace->last_ordinal;
        count -= (size_t)trace->last_ordinal;
        after = page.next;
    }
    return ZOVA_PLUGIN_OK;
}

/* Node row: zova_node_key, label, name, qualified_name, file_path, start_line,
 * end_line, properties. */
static int32_t ZOVA_PLUGIN_CALL plugin_node_row(void *user_data,
                                                const zova_plugin_value_v1 *values,
                                                uint64_t count) {
    static const int text_column[PLUGIN_TEXT_COLUMNS] = {1, 2, 3, 4, 7};
    plugin_trace_t *trace = user_data;
    if (count != PLUGIN_NODE_COLUMNS || values[0].kind != ZOVA_PLUGIN_VALUE_INTEGER ||
        values[5].kind != ZOVA_PLUGIN_VALUE_INTEGER ||
        values[6].kind != ZOVA_PLUGIN_VALUE_INTEGER) {
        return ZOVA_PLUGIN_ERROR;
    }
    if (trace->count == trace->cap) {
        size_t cap = trace->cap ? trace->cap * 2 : 64;
        plugin_row_t *rows = realloc(trace->rows, cap * sizeof(*rows));
        if (!rows) return ZOVA_PLUGIN_OUT_OF_MEMORY;
        trace->rows = rows;
        trace->cap = cap;
    }
    plugin_row_t row = {
        .key = values[0].integer,
        .start_line = values[5].integer,
        .end_line = values[6].integer,
        .hop = trace->level,
    };
    uint64_t total = 0;
    for (int i = 0; i < PLUGIN_TEXT_COLUMNS; i++) {
        const zova_plugin_value_v1 *value = &values[text_column[i]];
        if (value->kind != ZOVA_PLUGIN_VALUE_TEXT) return ZOVA_PLUGIN_ERROR;
        row.off[i] = total;
        row.len[i] = value->bytes_len;
        total += value->bytes_len;
    }
    row.text = malloc(total ? (size_t)total : 1);
    if (!row.text) return ZOVA_PLUGIN_OUT_OF_MEMORY;
    for (int i = 0; i < PLUGIN_TEXT_COLUMNS; i++) {
        if (row.len[i]) {
            memcpy(row.text + row.off[i], values[text_column[i]].bytes, (size_t)row.len[i]);
        }
    }
    trace->rows[trace->count++] = row;
    return ZOVA_PLUGIN_OK;
}

static void plugin_rows_truncate(plugin_trace_t *trace, size_t count) {
    while (trace->count > count) free(trace->rows[--trace->count].text);
}

/* Loads the cbm_nodes_v1 rows of keys[first..first+count). Graph nodes without a
 * row are skipped. A batch over the query byte budget is retried in halves. */
static int32_t plugin_hydrate(const plugin_session_t *session, plugin_trace_t *trace,
                              size_t first, size_t count) {
    static const char head[] = PLUGIN_NODE_SELECT "WHERE n.zova_node_key IN (";
    char sql[sizeof(head) + PLUGIN_HYDRATE_BATCH * 2 + 2];
    zova_plugin_value_v1 parameters[PLUGIN_HYDRATE_BATCH];
    size_t batch = PLUGIN_HYDRATE_BATCH;
    while (count > 0) {
        size_t n = count < batch ? count : batch;
        size_t len = sizeof(head) - 1;
        memcpy(sql, head, len);
        memset(parameters, 0, n * sizeof(parameters[0]));
        for (size_t i = 0; i < n; i++) {
            if (i > 0) sql[len++] = ',';
            sql[len++] = '?';
            parameters[i].kind = ZOVA_PLUGIN_VALUE_INTEGER;
            parameters[i].integer = trace->keys[first + i];
        }
        sql[len++] = ')';
        zova_plugin_query_request_v1 request = {
            .struct_size = sizeof(request),
            .sql = sql,
            .sql_len = len,
            .parameters = parameters,
            .parameter_count = n,
            .row_limit = n,
            .byte_limit = PLUGIN_PAGE_BYTES,
            .row = plugin_node_row,
            .user_data = trace,
        };
        size_t before = trace->count;
        int32_t status = session->query->query(session->connection, &request);
        if (status == ZOVA_PLUGIN_LIMIT && n > 1) {
            plugin_rows_truncate(trace, before);
            batch = n / 2;
            continue;
        }
        if (status != ZOVA_PLUGIN_OK) return status;
        first += n;
        count -= n;
    }
    return ZOVA_PLUGIN_OK;
}

static int plugin_bytes_compare(const plugin_row_t *a, const plugin_row_t *b, int column) {
    uint64_t shared = a->len[column] < b->len[column] ? a->len[column] : b->len[column];
    int order = shared ? memcmp(a->text + a->off[column], b->text + b->off[column], (size_t)shared)
                       : 0;
    if (order != 0) return order;
    return (a->len[column] > b->len[column]) - (a->len[column] < b->len[column]);
}

static int plugin_row_compare(const void *left, const void *right) {
    static const int order_columns[] = {ROW_FILE, ROW_QN, ROW_LABEL};
    const plugin_row_t *a = left;
    const plugin_row_t *b = right;
    if (a->hop != b->hop) return (a->hop > b->hop) - (a->hop < b->hop);
    for (size_t i = 0; i < sizeof(order_columns) / sizeof(order_columns[0]); i++) {
        int order = plugin_bytes_compare(a, b, order_columns[i]);
        if (order != 0) return order;
    }
    if (a->start_line != b->start_line) return (a->start_line > b->start_line) ? 1 : -1;
    if (a->end_line != b->end_line) return (a->end_line > b->end_line) ? 1 : -1;
    return (a->key > b->key) - (a->key < b->key);
}

/* Splits "A,B" into at most PLUGIN_EDGE_TYPES_MAX non-empty names. An empty
 * list yields 0; a malformed one -1. */
static int plugin_edge_types(const zova_plugin_value_v1 *value, zova_plugin_bytes_v1 *out) {
    int count = 0;
    if (value->bytes_len == 0) return 0;
    uint64_t start = 0;
    for (uint64_t i = 0; i <= value->bytes_len; i++) {
        if (i < value->bytes_len && value->bytes[i] != ',') continue;
        if (i == start || i - start > PLUGIN_NAME_MAX || count == PLUGIN_EDGE_TYPES_MAX) return -1;
        out[count++] = (zova_plugin_bytes_v1){value->bytes + start, i - start};
        start = i + 1;
    }
    return count;
}

static int plugin_text_equals(const zova_plugin_value_v1 *value, const char *text) {
    size_t len = strlen(text);
    return value->bytes_len == len && memcmp(value->bytes, text, len) == 0;
}

/* No names means every edge type. */
static void plugin_request_edge_types(zova_plugin_data_request_v1 *request,
                                      const zova_plugin_bytes_v1 *edge_types, int count) {
    if (count == 1) {
        request->edge_type = edge_types[0];
    } else if (count > 1) {
        request->ids = edge_types;
        request->id_count = (uint64_t)count;
    }
}

static int plugin_direction(const zova_plugin_value_v1 *value, uint32_t *out) {
    if (plugin_text_equals(value, "outbound")) {
        *out = ZOVA_PLUGIN_OUTGOING;
    } else if (plugin_text_equals(value, "inbound")) {
        *out = ZOVA_PLUGIN_INCOMING;
    } else {
        return -1;
    }
    return 0;
}


/* Walks breadth-first from start_key and leaves the reached nodes in
 * trace->rows, ordered and cut to max_results. */
static int32_t plugin_trace_run(const plugin_session_t *session, plugin_trace_t *trace,
                                zova_plugin_bytes_v1 graph, int64_t start_key,
                                uint32_t direction, const zova_plugin_bytes_v1 *edge_types,
                                int edge_type_count, int64_t max_depth, int64_t max_results) {
    if (plugin_visit(trace, start_key) < 0) return ZOVA_PLUGIN_OUT_OF_MEMORY;
    zova_plugin_data_request_v1 request = {
        .struct_size = sizeof(request),
        .operation = ZOVA_PLUGIN_GRAPH_NEIGHBORS_KEYED,
        .name = graph,
        .direction = direction,
        .row_limit = PLUGIN_PAGE_ROWS,
        .byte_limit = PLUGIN_PAGE_BYTES,
        .row = plugin_neighbor_row,
        .user_data = trace,
    };
    plugin_request_edge_types(&request, edge_types, edge_type_count);

    size_t level_start = 0;
    size_t level_end = 1;
    for (int64_t hop = 1; hop <= max_depth && level_start < level_end; hop++) {
        for (size_t first = level_start; first < level_end; first += PLUGIN_FRONTIER_BATCH) {
            size_t count = level_end - first;
            if (count > PLUGIN_FRONTIER_BATCH) count = PLUGIN_FRONTIER_BATCH;
            int32_t status = plugin_expand(session, trace, &request, first, count);
            if (status != ZOVA_PLUGIN_OK) return status;
        }
        level_start = level_end;
        level_end = trace->key_count;
        trace->level = (int32_t)hop;
        int32_t status = plugin_hydrate(session, trace, level_start, level_end - level_start);
        if (status != ZOVA_PLUGIN_OK) return status;
        /* Later levels sort after this one, so they cannot enter the result. */
        if (trace->count >= (size_t)max_results) break;
    }
    if (trace->count > 1) qsort(trace->rows, trace->count, sizeof(*trace->rows), plugin_row_compare);
    plugin_rows_truncate(trace, (size_t)max_results);
    return ZOVA_PLUGIN_OK;
}

static int32_t ZOVA_PLUGIN_CALL plugin_trace_open(const zova_plugin_host_v1 *host,
                                                  void *connection, void *state,
                                                  const zova_plugin_value_v1 *arguments,
                                                  uint64_t argument_count, void **out_cursor) {
    (void)state;
    *out_cursor = NULL;
    if (argument_count != PLUGIN_TRACE_ARGUMENTS) return ZOVA_PLUGIN_INVALID_ARGUMENT;
    zova_plugin_bytes_v1 graph = {arguments[0].bytes, arguments[0].bytes_len};
    int64_t start_key = arguments[1].integer;
    int64_t max_depth = arguments[4].integer;
    int64_t max_results = arguments[5].integer;
    zova_plugin_bytes_v1 edge_types[PLUGIN_EDGE_TYPES_MAX];
    int edge_type_count = plugin_edge_types(&arguments[3], edge_types);
    uint32_t direction;
    if (plugin_direction(&arguments[2], &direction) != 0 || graph.len == 0 ||
        graph.len > PLUGIN_NAME_MAX || start_key <= 0 || edge_type_count <= 0 || max_depth < 0 ||
        max_results <= 0 || max_results > PLUGIN_MAX_RESULTS) {
        return ZOVA_PLUGIN_INVALID_ARGUMENT;
    }
    plugin_session_t session;
    int32_t status = plugin_session_open(host, connection, &session);
    if (status != ZOVA_PLUGIN_OK) return status;
    plugin_trace_t *trace = calloc(1, sizeof(*trace));
    if (!trace) return ZOVA_PLUGIN_OUT_OF_MEMORY;
    *out_cursor = trace;
    return plugin_trace_run(&session, trace, graph, start_key, direction, edge_types,
                            edge_type_count, max_depth, max_results);
}

static int32_t ZOVA_PLUGIN_CALL plugin_trace_next(const zova_plugin_host_v1 *host,
                                                  void *connection, void *cursor,
                                                  zova_plugin_row_v1 row, void *row_context,
                                                  uint32_t *out_has_row) {
    (void)host;
    (void)connection;
    plugin_trace_t *trace = cursor;
    *out_has_row = 0;
    if (trace->next >= trace->count) return ZOVA_PLUGIN_OK;
    const plugin_row_t *item = &trace->rows[trace->next++];
    zova_plugin_value_v1 values[PLUGIN_TRACE_COLUMNS] = {0};
    values[0] = (zova_plugin_value_v1){.kind = ZOVA_PLUGIN_VALUE_INTEGER, .integer = item->key};
    values[1] = (zova_plugin_value_v1){.kind = ZOVA_PLUGIN_VALUE_INTEGER, .integer = item->hop};
    static const int text_column[PLUGIN_TEXT_COLUMNS] = {2, 3, 4, 5, 8};
    for (int i = 0; i < PLUGIN_TEXT_COLUMNS; i++) {
        values[text_column[i]] = (zova_plugin_value_v1){
            .kind = ZOVA_PLUGIN_VALUE_TEXT,
            .bytes = (const uint8_t *)item->text + item->off[i],
            .bytes_len = item->len[i],
        };
    }
    values[6] = (zova_plugin_value_v1){.kind = ZOVA_PLUGIN_VALUE_INTEGER,
                                       .integer = item->start_line};
    values[7] = (zova_plugin_value_v1){.kind = ZOVA_PLUGIN_VALUE_INTEGER,
                                       .integer = item->end_line};
    *out_has_row = 1;
    return row(row_context, values, PLUGIN_TRACE_COLUMNS);
}

static void ZOVA_PLUGIN_CALL plugin_trace_close(void *cursor) {
    plugin_trace_free(cursor);
}

/* ── edges ─────────────────────────────────────────────────────── */

/* One adjacency entry; type is copied because row bytes are borrowed. */
typedef struct {
    int64_t edge_key;
    int64_t neighbor_key;
    char *type;
    uint64_t type_len;
} plugin_neighbor_t;

/* One cbm edge. text holds local_name then properties. A graph edge carries
 * one or more of these in its payload. */
typedef struct {
    size_t neighbor;
    char *text;
    uint64_t local_name_len;
    uint64_t properties_len;
} plugin_edge_t;

typedef struct {
    plugin_neighbor_t *neighbors;
    size_t neighbor_count;
    size_t neighbor_cap;
    plugin_edge_t *edges;
    size_t edge_count;
    size_t edge_cap;
    size_t next;
    int64_t node_key;
    uint32_t direction;
    /* First neighbor of the payload batch being read, and the one being decoded. */
    size_t batch_first;
    size_t batch_count;
    size_t current;
} plugin_edges_t;

static void plugin_edges_truncate(plugin_edges_t *state, size_t count) {
    while (state->edge_count > count) free(state->edges[--state->edge_count].text);
}

static void plugin_edges_free(plugin_edges_t *state) {
    if (!state) return;
    plugin_edges_truncate(state, 0);
    for (size_t i = 0; i < state->neighbor_count; i++) free(state->neighbors[i].type);
    free(state->neighbors);
    free(state->edges);
    free(state);
}

static int32_t ZOVA_PLUGIN_CALL plugin_edges_neighbor_row(void *user_data,
                                                          const zova_plugin_value_v1 *values,
                                                          uint64_t count) {
    plugin_edges_t *state = user_data;
    if (count != PLUGIN_NEIGHBOR_COLUMNS || values[2].kind != ZOVA_PLUGIN_VALUE_INTEGER ||
        values[3].kind != ZOVA_PLUGIN_VALUE_INTEGER || values[4].kind != ZOVA_PLUGIN_VALUE_TEXT) {
        return ZOVA_PLUGIN_ERROR;
    }
    if (state->neighbor_count == state->neighbor_cap) {
        size_t cap = state->neighbor_cap ? state->neighbor_cap * 2 : 64;
        plugin_neighbor_t *neighbors = realloc(state->neighbors, cap * sizeof(*neighbors));
        if (!neighbors) return ZOVA_PLUGIN_OUT_OF_MEMORY;
        state->neighbors = neighbors;
        state->neighbor_cap = cap;
    }
    char *type = malloc(values[4].bytes_len ? (size_t)values[4].bytes_len : 1);
    if (!type) return ZOVA_PLUGIN_OUT_OF_MEMORY;
    if (values[4].bytes_len) memcpy(type, values[4].bytes, (size_t)values[4].bytes_len);
    state->neighbors[state->neighbor_count++] = (plugin_neighbor_t){
        .edge_key = values[2].integer,
        .neighbor_key = values[3].integer,
        .type = type,
        .type_len = values[4].bytes_len,
    };
    return ZOVA_PLUGIN_OK;
}

static int plugin_edges_record(const cbm_zova_edge_payload_record_t *record, void *context) {
    plugin_edges_t *state = context;
    if (state->edge_count == state->edge_cap) {
        size_t cap = state->edge_cap ? state->edge_cap * 2 : 64;
        plugin_edge_t *edges = realloc(state->edges, cap * sizeof(*edges));
        if (!edges) return -1;
        state->edges = edges;
        state->edge_cap = cap;
    }
    size_t total = record->local_name.len + record->properties.len;
    char *text = malloc(total ? total : 1);
    if (!text) return -1;
    if (record->local_name.len) memcpy(text, record->local_name.data, record->local_name.len);
    if (record->properties.len) {
        memcpy(text + record->local_name.len, record->properties.data, record->properties.len);
    }
    state->edges[state->edge_count++] = (plugin_edge_t){
        .neighbor = state->current,
        .text = text,
        .local_name_len = record->local_name.len,
        .properties_len = record->properties.len,
    };
    return 0;
}

/* EDGE_PAYLOADS_GET row: ordinal, found, input_key, payload. */
static int32_t ZOVA_PLUGIN_CALL plugin_edges_payload_row(void *user_data,
                                                         const zova_plugin_value_v1 *values,
                                                         uint64_t count) {
    plugin_edges_t *state = user_data;
    if (count != PLUGIN_PAYLOAD_COLUMNS || values[0].kind != ZOVA_PLUGIN_VALUE_INTEGER ||
        values[0].integer < 0 || (uint64_t)values[0].integer >= state->batch_count ||
        values[1].kind != ZOVA_PLUGIN_VALUE_INTEGER || values[1].integer != 1 ||
        values[3].kind != ZOVA_PLUGIN_VALUE_BLOB) {
        return ZOVA_PLUGIN_ERROR;
    }
    state->current = state->batch_first + (size_t)values[0].integer;
    if (values[2].integer != state->neighbors[state->current].edge_key) return ZOVA_PLUGIN_ERROR;
    return cbm_zova_edge_payload_visit(values[3].bytes, (size_t)values[3].bytes_len,
                                       plugin_edges_record, state, NULL) == 0
               ? ZOVA_PLUGIN_OK
               : ZOVA_PLUGIN_ERROR;
}

/* Decodes the payloads of every collected neighbor edge. A batch over the byte
 * budget is retried in halves. */
static int32_t plugin_edges_payloads(const plugin_session_t *session, plugin_edges_t *state,
                                     zova_plugin_bytes_v1 graph) {
    int64_t keys[PLUGIN_PAYLOAD_BATCH];
    size_t batch = PLUGIN_PAYLOAD_BATCH;
    size_t first = 0;
    while (first < state->neighbor_count) {
        size_t n = state->neighbor_count - first;
        if (n > batch) n = batch;
        for (size_t i = 0; i < n; i++) keys[i] = state->neighbors[first + i].edge_key;
        zova_plugin_data_request_v1 request = {
            .struct_size = sizeof(request),
            .operation = ZOVA_PLUGIN_GRAPH_EDGE_PAYLOADS_GET,
            .name = graph,
            .keys = keys,
            .key_count = n,
            .row_limit = n,
            .byte_limit = PLUGIN_PAGE_BYTES,
            .row = plugin_edges_payload_row,
            .user_data = state,
        };
        zova_plugin_data_page_v1 page = {0};
        size_t before = state->edge_count;
        state->batch_first = first;
        state->batch_count = n;
        int32_t status = session->data->read(session->connection, &request, &page);
        if (status == ZOVA_PLUGIN_LIMIT && n > 1) {
            plugin_edges_truncate(state, before);
            batch = n / 2;
            continue;
        }
        if (status != ZOVA_PLUGIN_OK) return status;
        if (page.rows != n) return ZOVA_PLUGIN_ERROR;
        first += n;
    }
    return ZOVA_PLUGIN_OK;
}

/* Collects the adjacency of node_key into state->neighbors. */
static int32_t plugin_edges_neighbors(const plugin_session_t *session, plugin_edges_t *state,
                                      zova_plugin_bytes_v1 graph, int64_t node_key,
                                      uint32_t direction, const zova_plugin_bytes_v1 *edge_types,
                                      int edge_type_count) {
    state->node_key = node_key;
    state->direction = direction;
    zova_plugin_data_request_v1 request = {
        .struct_size = sizeof(request),
        .operation = ZOVA_PLUGIN_GRAPH_NEIGHBORS_KEYED,
        .name = graph,
        .keys = &state->node_key,
        .key_count = 1,
        .direction = direction,
        .row_limit = PLUGIN_PAGE_ROWS,
        .byte_limit = PLUGIN_PAGE_BYTES,
        .row = plugin_edges_neighbor_row,
        .user_data = state,
    };
    plugin_request_edge_types(&request, edge_types, edge_type_count);
    for (;;) {
        zova_plugin_data_page_v1 page = {0};
        int32_t status = session->data->read(session->connection, &request, &page);
        if (status != ZOVA_PLUGIN_OK) return status;
        if (!page.has_more) return ZOVA_PLUGIN_OK;
        if (page.rows == 0) return ZOVA_PLUGIN_ERROR;
        request.after = page.next;
    }
}

static int32_t ZOVA_PLUGIN_CALL plugin_edges_open(const zova_plugin_host_v1 *host,
                                                  void *connection, void *unused,
                                                  const zova_plugin_value_v1 *arguments,
                                                  uint64_t argument_count, void **out_cursor) {
    (void)unused;
    *out_cursor = NULL;
    if (argument_count != PLUGIN_EDGES_ARGUMENTS) return ZOVA_PLUGIN_INVALID_ARGUMENT;
    zova_plugin_bytes_v1 graph = {arguments[0].bytes, arguments[0].bytes_len};
    zova_plugin_bytes_v1 edge_types[PLUGIN_EDGE_TYPES_MAX];
    int edge_type_count = plugin_edge_types(&arguments[3], edge_types);
    uint32_t direction;
    if (plugin_direction(&arguments[2], &direction) != 0 || graph.len == 0 ||
        graph.len > PLUGIN_NAME_MAX || arguments[1].integer <= 0 || edge_type_count < 0) {
        return ZOVA_PLUGIN_INVALID_ARGUMENT;
    }
    plugin_session_t session;
    int32_t status = plugin_session_open(host, connection, &session);
    if (status != ZOVA_PLUGIN_OK) return status;
    plugin_edges_t *state = calloc(1, sizeof(*state));
    if (!state) return ZOVA_PLUGIN_OUT_OF_MEMORY;
    *out_cursor = state;
    status = plugin_edges_neighbors(&session, state, graph, arguments[1].integer, direction,
                                    edge_types, edge_type_count);
    return status == ZOVA_PLUGIN_OK ? plugin_edges_payloads(&session, state, graph) : status;
}

static int32_t ZOVA_PLUGIN_CALL plugin_edges_next(const zova_plugin_host_v1 *host,
                                                  void *connection, void *cursor,
                                                  zova_plugin_row_v1 row, void *row_context,
                                                  uint32_t *out_has_row) {
    (void)host;
    (void)connection;
    plugin_edges_t *state = cursor;
    *out_has_row = 0;
    if (state->next >= state->edge_count) return ZOVA_PLUGIN_OK;
    const plugin_edge_t *edge = &state->edges[state->next++];
    const plugin_neighbor_t *neighbor = &state->neighbors[edge->neighbor];
    int outgoing = state->direction == ZOVA_PLUGIN_OUTGOING;
    const uint8_t *text = (const uint8_t *)edge->text;
    zova_plugin_value_v1 values[PLUGIN_EDGES_COLUMNS] = {
        {.kind = ZOVA_PLUGIN_VALUE_INTEGER, .integer = neighbor->edge_key},
        {.kind = ZOVA_PLUGIN_VALUE_INTEGER,
         .integer = outgoing ? state->node_key : neighbor->neighbor_key},
        {.kind = ZOVA_PLUGIN_VALUE_INTEGER,
         .integer = outgoing ? neighbor->neighbor_key : state->node_key},
        {.kind = ZOVA_PLUGIN_VALUE_TEXT, .bytes = (const uint8_t *)neighbor->type,
         .bytes_len = neighbor->type_len},
        {.kind = ZOVA_PLUGIN_VALUE_TEXT, .bytes = text, .bytes_len = edge->local_name_len},
        {.kind = ZOVA_PLUGIN_VALUE_TEXT, .bytes = text + edge->local_name_len,
         .bytes_len = edge->properties_len},
    };
    *out_has_row = 1;
    return row(row_context, values, PLUGIN_EDGES_COLUMNS);
}

static void ZOVA_PLUGIN_CALL plugin_edges_close(void *cursor) {
    plugin_edges_free(cursor);
}

/* ── cypher ────────────────────────────────────────────────────── */

/* Store backend for one workspace: answers the reads the Cypher engine makes
 * from the plugin services. Node ids are Zova node keys and edge ids Zova edge
 * keys, so id(n) and id(r) differ from the host store's hashed ids. */
typedef struct {
    plugin_session_t session;
    zova_plugin_bytes_v1 graph;
    char graph_name[CBM_ZOVA_WORKSPACE_ID_MAX + 32];
    int64_t workspace_key;
    char *project;
} plugin_store_t;

static char *plugin_dup(const char *bytes, uint64_t len) {
    char *copy = malloc((size_t)len + 1);
    if (!copy) return NULL;
    if (len) memcpy(copy, bytes, (size_t)len);
    copy[len] = '\0';
    return copy;
}

static void plugin_node_clear(cbm_node_t *node) {
    free((char *)node->project);
    free((char *)node->label);
    free((char *)node->name);
    free((char *)node->qualified_name);
    free((char *)node->file_path);
    free((char *)node->properties_json);
    memset(node, 0, sizeof(*node));
}

static int plugin_node_from_row(const plugin_store_t *store, const plugin_row_t *row,
                                cbm_node_t *out) {
    *out = (cbm_node_t){
        .id = row->key,
        .project = plugin_dup(store->project, strlen(store->project)),
        .label = plugin_dup(row->text + row->off[ROW_LABEL], row->len[ROW_LABEL]),
        .name = plugin_dup(row->text + row->off[ROW_NAME], row->len[ROW_NAME]),
        .qualified_name = plugin_dup(row->text + row->off[ROW_QN], row->len[ROW_QN]),
        .file_path = plugin_dup(row->text + row->off[ROW_FILE], row->len[ROW_FILE]),
        .start_line = (int)row->start_line,
        .end_line = (int)row->end_line,
        .properties_json = plugin_dup(row->text + row->off[ROW_PROPERTIES],
                                      row->len[ROW_PROPERTIES]),
    };
    if (out->project && out->label && out->name && out->qualified_name && out->file_path &&
        out->properties_json) {
        return 0;
    }
    plugin_node_clear(out);
    return -1;
}

static zova_plugin_value_v1 plugin_text_value(const char *bytes, uint64_t len) {
    return (zova_plugin_value_v1){.kind = ZOVA_PLUGIN_VALUE_TEXT,
                                  .bytes = (const uint8_t *)bytes, .bytes_len = len};
}

/* Workspace nodes ordered by name, qualified_name, key, optionally of one label.
 * Read in keyset pages; a page over the byte budget is retried at half size. */
static int plugin_store_search(void *ctx, const cbm_search_params_t *params,
                               cbm_search_output_t *out) {
    static const char sql[] = PLUGIN_NODE_SELECT
        "WHERE n.workspace_key=?1 AND (?2='' OR n.label=?2) AND "
        "(?3=0 OR (n.name,n.qualified_name,n.zova_node_key)>(?4,?5,?6)) "
        "ORDER BY n.name,n.qualified_name,n.zova_node_key LIMIT ?7";
    const plugin_store_t *store = ctx;
    memset(out, 0, sizeof(*out));
    /* The engine only scans by label; refuse filters this backend does not apply. */
    if (params->name_pattern || params->qn_pattern || params->file_pattern ||
        params->relationship || params->exclude_labels || params->exclude_entry_points ||
        params->min_degree >= 0 || params->max_degree >= 0 || params->offset != 0) {
        return CBM_STORE_ERR;
    }
    size_t limit = params->limit > 0 ? (size_t)params->limit : 10;
    const char *label = params->label ? params->label : "";
    plugin_trace_t *rows = calloc(1, sizeof(*rows));
    if (!rows) return CBM_STORE_ERR;
    size_t page = PLUGIN_SCAN_PAGE;
    int rc = CBM_STORE_OK;
    while (rows->count < limit) {
        size_t n = limit - rows->count < page ? limit - rows->count : page;
        const plugin_row_t *last = rows->count ? &rows->rows[rows->count - 1] : NULL;
        zova_plugin_value_v1 parameters[] = {
            {.kind = ZOVA_PLUGIN_VALUE_INTEGER, .integer = store->workspace_key},
            plugin_text_value(label, strlen(label)),
            {.kind = ZOVA_PLUGIN_VALUE_INTEGER, .integer = last != NULL},
            plugin_text_value(last ? last->text + last->off[ROW_NAME] : "",
                              last ? last->len[ROW_NAME] : 0),
            plugin_text_value(last ? last->text + last->off[ROW_QN] : "",
                              last ? last->len[ROW_QN] : 0),
            {.kind = ZOVA_PLUGIN_VALUE_INTEGER, .integer = last ? last->key : 0},
            {.kind = ZOVA_PLUGIN_VALUE_INTEGER, .integer = (int64_t)n},
        };
        zova_plugin_query_request_v1 request = {
            .struct_size = sizeof(request),
            .sql = sql,
            .sql_len = sizeof(sql) - 1,
            .parameters = parameters,
            .parameter_count = sizeof(parameters) / sizeof(parameters[0]),
            .row_limit = n,
            .byte_limit = PLUGIN_PAGE_BYTES,
            .row = plugin_node_row,
            .user_data = rows,
        };
        size_t before = rows->count;
        int32_t status = store->session.query->query(store->session.connection, &request);
        if (status == ZOVA_PLUGIN_LIMIT && n > 1) {
            plugin_rows_truncate(rows, before);
            page = n / 2;
            continue;
        }
        if (status != ZOVA_PLUGIN_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
        if (rows->count - before < n) break;
    }
    cbm_search_result_t *results =
        rc == CBM_STORE_OK && rows->count ? calloc(rows->count, sizeof(*results)) : NULL;
    if (rc == CBM_STORE_OK && rows->count && !results) rc = CBM_STORE_ERR;
    for (size_t i = 0; rc == CBM_STORE_OK && i < rows->count; i++) {
        if (plugin_node_from_row(store, &rows->rows[i], &results[i].node) != 0) {
            rc = CBM_STORE_ERR;
            break;
        }
        out->count = (int)i + 1;
    }
    out->results = results;
    out->total = out->count;
    if (rc != CBM_STORE_OK) cbm_store_search_free(out);
    plugin_trace_free(rows);
    return rc;
}

static int plugin_store_find_node_by_id(void *ctx, int64_t id, cbm_node_t *out) {
    const plugin_store_t *store = ctx;
    memset(out, 0, sizeof(*out));
    if (id <= 0) return CBM_STORE_NOT_FOUND;
    plugin_trace_t *rows = calloc(1, sizeof(*rows));
    if (!rows) return CBM_STORE_ERR;
    int rc = CBM_STORE_ERR;
    if (plugin_visit(rows, id) == 1 &&
        plugin_hydrate(&store->session, rows, 0, 1) == ZOVA_PLUGIN_OK) {
        rc = rows->count == 0 ? CBM_STORE_NOT_FOUND
                              : plugin_node_from_row(store, &rows->rows[0], out) == 0
                                    ? CBM_STORE_OK
                                    : CBM_STORE_ERR;
    }
    plugin_trace_free(rows);
    return rc;
}

static int plugin_store_direction(const char *direction, uint32_t *out) {
    if (!direction || strcmp(direction, "outbound") == 0) {
        *out = ZOVA_PLUGIN_OUTGOING;
    } else if (strcmp(direction, "inbound") == 0) {
        *out = ZOVA_PLUGIN_INCOMING;
    } else {
        return -1;
    }
    return 0;
}

static int plugin_store_find_edges(void *ctx, int64_t node_id, const char *direction,
                                   const char *type, cbm_edge_t **out, int *count) {
    const plugin_store_t *store = ctx;
    *out = NULL;
    *count = 0;
    uint32_t zova_direction;
    if (node_id <= 0 || plugin_store_direction(direction, &zova_direction) != 0) {
        return CBM_STORE_ERR;
    }
    zova_plugin_bytes_v1 edge_type = {(const uint8_t *)type, type ? strlen(type) : 0};
    if (edge_type.len > PLUGIN_NAME_MAX) return CBM_STORE_ERR;
    plugin_edges_t *state = calloc(1, sizeof(*state));
    if (!state) return CBM_STORE_ERR;
    int rc = CBM_STORE_ERR;
    cbm_edge_t *edges = NULL;
    if (plugin_edges_neighbors(&store->session, state, store->graph, node_id, zova_direction,
                               &edge_type, edge_type.len ? 1 : 0) != ZOVA_PLUGIN_OK ||
        plugin_edges_payloads(&store->session, state, store->graph) != ZOVA_PLUGIN_OK) {
        goto done;
    }
    edges = state->edge_count ? calloc(state->edge_count, sizeof(*edges)) : NULL;
    if (state->edge_count && !edges) goto done;
    for (size_t i = 0; i < state->edge_count; i++) {
        const plugin_edge_t *edge = &state->edges[i];
        const plugin_neighbor_t *neighbor = &state->neighbors[edge->neighbor];
        int outgoing = zova_direction == ZOVA_PLUGIN_OUTGOING;
        edges[i] = (cbm_edge_t){
            .id = neighbor->edge_key,
            .project = plugin_dup(store->project, strlen(store->project)),
            .source_id = outgoing ? node_id : neighbor->neighbor_key,
            .target_id = outgoing ? neighbor->neighbor_key : node_id,
            .type = plugin_dup(neighbor->type, neighbor->type_len),
            .properties_json = plugin_dup(edge->text + edge->local_name_len,
                                          edge->properties_len),
        };
        *count = (int)i + 1;
        if (!edges[i].project || !edges[i].type || !edges[i].properties_json) goto done;
    }
    rc = CBM_STORE_OK;
done:
    if (rc == CBM_STORE_OK) {
        *out = edges;
    } else {
        cbm_store_free_edges(edges, *count);
        *count = 0;
    }
    plugin_edges_free(state);
    return rc;
}

/* CALLS edges in and out, like the host store. */
static int plugin_store_node_degree(void *ctx, int64_t node_id, int *in_deg, int *out_deg) {
    static const zova_plugin_bytes_v1 calls = {(const uint8_t *)"CALLS", 5};
    const plugin_store_t *store = ctx;
    int *degrees[] = {out_deg, in_deg};
    const uint32_t directions[] = {ZOVA_PLUGIN_OUTGOING, ZOVA_PLUGIN_INCOMING};
    if (node_id <= 0) return CBM_STORE_ERR;
    for (int i = 0; i < 2; i++) {
        plugin_edges_t *state = calloc(1, sizeof(*state));
        if (!state) return CBM_STORE_ERR;
        int32_t status = plugin_edges_neighbors(&store->session, state, store->graph, node_id,
                                                directions[i], &calls, 1);
        *degrees[i] = (int)state->neighbor_count;
        plugin_edges_free(state);
        if (status != ZOVA_PLUGIN_OK) return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static int plugin_store_bfs(void *ctx, int64_t start_id, const char *direction,
                            const char **edge_types, int edge_type_count, int max_depth,
                            int max_results, cbm_traverse_result_t *out) {
    static const char *const default_types[] = {"CALLS"};
    const plugin_store_t *store = ctx;
    memset(out, 0, sizeof(*out));
    uint32_t zova_direction;
    /* The host store walks one direction per call too. */
    if (plugin_store_direction(direction, &zova_direction) != 0) return CBM_STORE_NOT_FOUND;
    if (edge_type_count <= 0) {
        edge_types = (const char **)default_types;
        edge_type_count = 1;
    }
    if (edge_type_count > PLUGIN_EDGE_TYPES_MAX || max_depth < 0 || max_results <= 0 ||
        max_results > PLUGIN_MAX_RESULTS) {
        return CBM_STORE_ERR;
    }
    zova_plugin_bytes_v1 types[PLUGIN_EDGE_TYPES_MAX];
    for (int i = 0; i < edge_type_count; i++) {
        types[i] = (zova_plugin_bytes_v1){(const uint8_t *)edge_types[i], strlen(edge_types[i])};
        if (types[i].len == 0 || types[i].len > PLUGIN_NAME_MAX) return CBM_STORE_ERR;
    }
    int rc = plugin_store_find_node_by_id(ctx, start_id, &out->root);
    if (rc != CBM_STORE_OK) return rc;
    plugin_trace_t *trace = calloc(1, sizeof(*trace));
    rc = CBM_STORE_ERR;
    if (trace && plugin_trace_run(&store->session, trace, store->graph, start_id, zova_direction,
                                  types, edge_type_count, max_depth, max_results) ==
                     ZOVA_PLUGIN_OK) {
        out->visited = trace->count ? calloc(trace->count, sizeof(*out->visited)) : NULL;
        rc = trace->count && !out->visited ? CBM_STORE_ERR : CBM_STORE_OK;
        for (size_t i = 0; rc == CBM_STORE_OK && i < trace->count; i++) {
            if (plugin_node_from_row(store, &trace->rows[i], &out->visited[i].node) != 0) {
                rc = CBM_STORE_ERR;
                break;
            }
            out->visited[i].hop = trace->rows[i].hop;
            out->visited_count = (int)i + 1;
        }
    }
    plugin_trace_free(trace);
    if (rc != CBM_STORE_OK) cbm_store_traverse_free(out);
    return rc;
}

static const cbm_store_backend_t plugin_store_backend = {
    .search = plugin_store_search,
    .find_node_by_id = plugin_store_find_node_by_id,
    .find_edges = plugin_store_find_edges,
    .node_degree = plugin_store_node_degree,
    .bfs = plugin_store_bfs,
};

/* Workspace row: workspace_key, project. */
static int32_t ZOVA_PLUGIN_CALL plugin_workspace_row(void *user_data,
                                                     const zova_plugin_value_v1 *values,
                                                     uint64_t count) {
    plugin_store_t *store = user_data;
    if (count != 2 || values[0].kind != ZOVA_PLUGIN_VALUE_INTEGER ||
        values[1].kind != ZOVA_PLUGIN_VALUE_TEXT) {
        return ZOVA_PLUGIN_ERROR;
    }
    store->workspace_key = values[0].integer;
    store->project = plugin_dup((const char *)values[1].bytes, values[1].bytes_len);
    return store->project ? ZOVA_PLUGIN_OK : ZOVA_PLUGIN_OUT_OF_MEMORY;
}

/* Result rows as (kind, cells) pairs, in emission order. */
typedef struct {
    const char **kinds;
    char **cells;
    size_t count;
    size_t next;
} plugin_cypher_t;

static void plugin_cypher_free(plugin_cypher_t *cursor) {
    if (!cursor) return;
    for (size_t i = 0; i < cursor->count; i++) free(cursor->cells[i]);
    free(cursor->kinds);
    free(cursor->cells);
    free(cursor);
}

/* Takes ownership of cells; NULL cells reports an allocation failure. */
static int plugin_cypher_push(plugin_cypher_t *cursor, const char *kind, char *cells) {
    if (!cells) return -1;
    cursor->kinds[cursor->count] = kind;
    cursor->cells[cursor->count++] = cells;
    return 0;
}

static char *plugin_json_array(const char *const *values, int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, array);
    for (int i = 0; i < count; i++) {
        if (values[i]) {
            yyjson_mut_arr_add_str(doc, array, values[i]);
        } else {
            yyjson_mut_arr_add_null(doc, array);
        }
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int32_t plugin_cypher_run(const plugin_session_t *session,
                                 const zova_plugin_value_v1 *arguments,
                                 plugin_cypher_t *cursor) {
    static const char workspace_sql[] =
        "SELECT r.workspace_key,p.project FROM cbm_workspace_registry r "
        "JOIN cbm_projects_v1 p ON p.workspace_key=r.workspace_key WHERE r.workspace_id=?1";
    plugin_store_t store = {.session = *session};
    char *workspace_id = plugin_dup((const char *)arguments[0].bytes, arguments[0].bytes_len);
    char *query = plugin_dup((const char *)arguments[1].bytes, arguments[1].bytes_len);
    cbm_store_t *handle = NULL;
    cbm_cypher_result_t result = {0};
    int32_t status = ZOVA_PLUGIN_OUT_OF_MEMORY;
    /* Up to three rows besides the result rows: columns, warning, error. */
    cursor->kinds = calloc(3, sizeof(*cursor->kinds));
    cursor->cells = calloc(3, sizeof(*cursor->cells));
    if (!workspace_id || !query || !cursor->kinds || !cursor->cells) goto done;
    if (strlen(workspace_id) != arguments[0].bytes_len || strlen(query) != arguments[1].bytes_len ||
        cbm_zova_workspace_graph_name(workspace_id, store.graph_name,
                                      sizeof(store.graph_name)) != 0) {
        status = ZOVA_PLUGIN_INVALID_ARGUMENT;
        goto done;
    }
    store.graph = (zova_plugin_bytes_v1){(const uint8_t *)store.graph_name,
                                         strlen(store.graph_name)};
    zova_plugin_query_request_v1 request = {
        .struct_size = sizeof(request),
        .sql = workspace_sql,
        .sql_len = sizeof(workspace_sql) - 1,
        .parameters = &arguments[0],
        .parameter_count = 1,
        .row_limit = 1,
        .byte_limit = PLUGIN_PAGE_BYTES,
        .row = plugin_workspace_row,
        .user_data = &store,
    };
    status = session->query->query(session->connection, &request);
    if (status != ZOVA_PLUGIN_OK) goto done;
    status = ZOVA_PLUGIN_OUT_OF_MEMORY;
    if (!store.project) {
        status = plugin_cypher_push(cursor, "error", plugin_dup("workspace not found", 19)) == 0
                     ? ZOVA_PLUGIN_OK
                     : ZOVA_PLUGIN_OUT_OF_MEMORY;
        goto done;
    }
    handle = cbm_store_open_backend(&plugin_store_backend, &store);
    if (!handle) goto done;
    if (cbm_cypher_execute(handle, query, store.project, (int)arguments[2].integer, &result) !=
        0) {
        const char *message = result.error ? result.error : "query failed";
        if (plugin_cypher_push(cursor, "error", plugin_dup(message, strlen(message))) == 0) {
            status = ZOVA_PLUGIN_OK;
        }
        goto done;
    }
    size_t capacity = (size_t)result.row_count + 3;
    const char **kinds = realloc(cursor->kinds, capacity * sizeof(*kinds));
    if (!kinds) goto done;
    cursor->kinds = kinds;
    char **cells = realloc(cursor->cells, capacity * sizeof(*cells));
    if (!cells) goto done;
    cursor->cells = cells;
    if (plugin_cypher_push(cursor, "columns",
                           plugin_json_array(result.columns, result.col_count)) != 0) {
        goto done;
    }
    for (int i = 0; i < result.row_count; i++) {
        if (plugin_cypher_push(cursor, "row",
                               plugin_json_array(result.rows[i], result.col_count)) != 0) {
            goto done;
        }
    }
    if (result.warning && plugin_cypher_push(cursor, "warning",
                                             plugin_dup(result.warning,
                                                        strlen(result.warning))) != 0) {
        goto done;
    }
    status = ZOVA_PLUGIN_OK;
done:
    cbm_cypher_result_free(&result);
    cbm_store_close(handle);
    free(store.project);
    free(workspace_id);
    free(query);
    return status;
}

static int32_t ZOVA_PLUGIN_CALL plugin_cypher_open(const zova_plugin_host_v1 *host,
                                                   void *connection, void *unused,
                                                   const zova_plugin_value_v1 *arguments,
                                                   uint64_t argument_count, void **out_cursor) {
    (void)unused;
    *out_cursor = NULL;
    if (argument_count != PLUGIN_CYPHER_ARGUMENTS || arguments[1].bytes_len == 0 ||
        arguments[2].integer < 0 || arguments[2].integer > INT32_MAX) {
        return ZOVA_PLUGIN_INVALID_ARGUMENT;
    }
    plugin_session_t session;
    int32_t status = plugin_session_open(host, connection, &session);
    if (status != ZOVA_PLUGIN_OK) return status;
    plugin_cypher_t *cursor = calloc(1, sizeof(*cursor));
    if (!cursor) return ZOVA_PLUGIN_OUT_OF_MEMORY;
    *out_cursor = cursor;
    return plugin_cypher_run(&session, arguments, cursor);
}

static int32_t ZOVA_PLUGIN_CALL plugin_cypher_next(const zova_plugin_host_v1 *host,
                                                   void *connection, void *raw,
                                                   zova_plugin_row_v1 row, void *row_context,
                                                   uint32_t *out_has_row) {
    (void)host;
    (void)connection;
    plugin_cypher_t *cursor = raw;
    *out_has_row = 0;
    if (cursor->next >= cursor->count) return ZOVA_PLUGIN_OK;
    const char *kind = cursor->kinds[cursor->next];
    const char *cells = cursor->cells[cursor->next++];
    zova_plugin_value_v1 values[PLUGIN_CYPHER_COLUMNS] = {
        plugin_text_value(kind, strlen(kind)),
        plugin_text_value(cells, strlen(cells)),
    };
    *out_has_row = 1;
    return row(row_context, values, PLUGIN_CYPHER_COLUMNS);
}

static void ZOVA_PLUGIN_CALL plugin_cypher_close(void *cursor) {
    plugin_cypher_free(cursor);
}

#define PLUGIN_COLUMN(text, kind) {{(const uint8_t *)(text), sizeof(text) - 1}, (kind), 0}

static int32_t ZOVA_PLUGIN_CALL plugin_register_sql(const zova_plugin_host_v1 *host,
                                                    void *connection) {
    static const zova_plugin_operation_column_v1 arguments[PLUGIN_TRACE_ARGUMENTS] = {
        PLUGIN_COLUMN("graph", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("start_key", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("direction", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("edge_types", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("max_depth", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("max_results", ZOVA_PLUGIN_VALUE_INTEGER),
    };
    static const zova_plugin_operation_column_v1 columns[PLUGIN_TRACE_COLUMNS] = {
        PLUGIN_COLUMN("node_key", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("hop", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("label", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("name", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("qualified_name", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("file_path", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("start_line", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("end_line", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("properties", ZOVA_PLUGIN_VALUE_TEXT),
    };
    const void *raw = NULL;
    int32_t status = plugin_service(host, connection, ZOVA_PLUGIN_SERVICE_OPERATIONS,
                                    sizeof(zova_plugin_operation_service_v1), &raw);
    if (status != ZOVA_PLUGIN_OK) return status;
    zova_plugin_operation_v1 operation = {
        .struct_size = sizeof(operation),
        .kind = ZOVA_PLUGIN_OPERATION_TABLE,
        .flags = ZOVA_PLUGIN_OPERATION_EXACT | ZOVA_PLUGIN_OPERATION_ORDERED,
        .name = {(const uint8_t *)"trace_path", sizeof("trace_path") - 1},
        .arguments = arguments,
        .argument_count = PLUGIN_TRACE_ARGUMENTS,
        .column_count = PLUGIN_TRACE_COLUMNS,
        .columns = columns,
        .open = plugin_trace_open,
        .next = plugin_trace_next,
        .close = plugin_trace_close,
    };
    const zova_plugin_operation_service_v1 *operations = raw;
    status = operations->register_operation(connection, &operation);
    if (status != ZOVA_PLUGIN_OK) return status;

    static const zova_plugin_operation_column_v1 edges_arguments[PLUGIN_EDGES_ARGUMENTS] = {
        PLUGIN_COLUMN("graph", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("node_key", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("direction", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("edge_types", ZOVA_PLUGIN_VALUE_TEXT),
    };
    static const zova_plugin_operation_column_v1 edges_columns[PLUGIN_EDGES_COLUMNS] = {
        PLUGIN_COLUMN("edge_key", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("source_key", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("target_key", ZOVA_PLUGIN_VALUE_INTEGER),
        PLUGIN_COLUMN("edge_type", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("local_name", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("properties", ZOVA_PLUGIN_VALUE_TEXT),
    };
    zova_plugin_operation_v1 edges = {
        .struct_size = sizeof(edges),
        .kind = ZOVA_PLUGIN_OPERATION_TABLE,
        .flags = ZOVA_PLUGIN_OPERATION_EXACT | ZOVA_PLUGIN_OPERATION_ORDERED,
        .name = {(const uint8_t *)"edges", sizeof("edges") - 1},
        .arguments = edges_arguments,
        .argument_count = PLUGIN_EDGES_ARGUMENTS,
        .column_count = PLUGIN_EDGES_COLUMNS,
        .columns = edges_columns,
        .open = plugin_edges_open,
        .next = plugin_edges_next,
        .close = plugin_edges_close,
    };
    status = operations->register_operation(connection, &edges);
    if (status != ZOVA_PLUGIN_OK) return status;

    static const zova_plugin_operation_column_v1 cypher_arguments[PLUGIN_CYPHER_ARGUMENTS] = {
        PLUGIN_COLUMN("workspace_id", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("query", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("max_rows", ZOVA_PLUGIN_VALUE_INTEGER),
    };
    static const zova_plugin_operation_column_v1 cypher_columns[PLUGIN_CYPHER_COLUMNS] = {
        PLUGIN_COLUMN("kind", ZOVA_PLUGIN_VALUE_TEXT),
        PLUGIN_COLUMN("cells", ZOVA_PLUGIN_VALUE_TEXT),
    };
    zova_plugin_operation_v1 cypher = {
        .struct_size = sizeof(cypher),
        .kind = ZOVA_PLUGIN_OPERATION_TABLE,
        .flags = ZOVA_PLUGIN_OPERATION_EXACT | ZOVA_PLUGIN_OPERATION_ORDERED,
        .name = {(const uint8_t *)"cypher", sizeof("cypher") - 1},
        .arguments = cypher_arguments,
        .argument_count = PLUGIN_CYPHER_ARGUMENTS,
        .column_count = PLUGIN_CYPHER_COLUMNS,
        .columns = cypher_columns,
        .open = plugin_cypher_open,
        .next = plugin_cypher_next,
        .close = plugin_cypher_close,
    };
    return operations->register_operation(connection, &cypher);
}

/* The plugin owns no tables. data_version names the cbm tables and graph layout
 * it reads (cbm_nodes_v1, cbm_files_v1, cbm_graph_<workspace>); a newer plugin
 * keeps opening older installs by listing them in accepted_data_versions. */
static const zova_plugin_compatibility_descriptor_v1 plugin_descriptor = {
    .base = {
        .base = {
            .struct_size = sizeof(zova_plugin_compatibility_descriptor_v1),
            .abi_version = ZOVA_PLUGIN_ABI_V1,
            .flags = ZOVA_PLUGIN_REQUIRES_QUERY_V1 | ZOVA_PLUGIN_REQUIRES_DATA_V1 |
                     ZOVA_PLUGIN_REQUIRES_OPERATIONS_V1 | ZOVA_PLUGIN_HAS_DATA_COMPATIBILITY_V1,
            .name = "cbm",
            .version = "0.1.0",
            .storage_prefix = "_zova_ext_cbm_",
            .zova_abi_min = "1.3.0",
            .capabilities = "",
            .register_sql = plugin_register_sql,
        },
    },
    .data_version = "1.0.0",
    .accepted_data_versions = "1.0.0",
};

const struct zova_plugin_descriptor_v1 *cbm_zova_plugin_descriptor(void) {
    return &plugin_descriptor.base.base;
}

#endif
