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

#include "zova_plugin.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
};

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
static int32_t plugin_expand(const zova_plugin_data_service_v1 *data, void *connection,
                             plugin_trace_t *trace, zova_plugin_data_request_v1 *request,
                             size_t first, size_t count) {
    zova_plugin_cursor_v1 after = {0};
    while (count > 0) {
        zova_plugin_data_page_v1 page = {0};
        /* The row callback may grow trace->keys; re-read the pointer per page. */
        request->keys = trace->keys + first;
        request->key_count = count;
        request->after = after;
        trace->last_ordinal = 0;
        int32_t status = data->read(connection, request, &page);
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
static int32_t plugin_hydrate(const zova_plugin_query_service_v1 *query, void *connection,
                              plugin_trace_t *trace, size_t first, size_t count) {
    static const char head[] =
        "SELECT n.zova_node_key,n.label,n.name,n.qualified_name,f.file_path,n.start_line,"
        "n.end_line,n.properties FROM cbm_nodes_v1 n JOIN cbm_files_v1 f "
        "ON f.file_key=n.file_key WHERE n.zova_node_key IN (";
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
        int32_t status = query->query(connection, &request);
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

/* Splits "A,B" into at most PLUGIN_EDGE_TYPES_MAX non-empty names. */
static int plugin_edge_types(const zova_plugin_value_v1 *value, zova_plugin_bytes_v1 *out) {
    int count = 0;
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

static int32_t ZOVA_PLUGIN_CALL plugin_trace_open(const zova_plugin_host_v1 *host,
                                                  void *connection, void *state,
                                                  const zova_plugin_value_v1 *arguments,
                                                  uint64_t argument_count, void **out_cursor) {
    (void)state;
    *out_cursor = NULL;
    if (argument_count != PLUGIN_TRACE_ARGUMENTS) return ZOVA_PLUGIN_INVALID_ARGUMENT;
    const zova_plugin_value_v1 *graph = &arguments[0];
    int64_t start_key = arguments[1].integer;
    int64_t max_depth = arguments[4].integer;
    int64_t max_results = arguments[5].integer;
    zova_plugin_bytes_v1 edge_types[PLUGIN_EDGE_TYPES_MAX];
    int edge_type_count = plugin_edge_types(&arguments[3], edge_types);
    uint32_t direction;
    if (plugin_text_equals(&arguments[2], "outbound")) {
        direction = ZOVA_PLUGIN_OUTGOING;
    } else if (plugin_text_equals(&arguments[2], "inbound")) {
        direction = ZOVA_PLUGIN_INCOMING;
    } else {
        return ZOVA_PLUGIN_INVALID_ARGUMENT;
    }
    if (graph->bytes_len == 0 || graph->bytes_len > PLUGIN_NAME_MAX || start_key <= 0 ||
        edge_type_count <= 0 || max_depth < 0 || max_results <= 0 ||
        max_results > PLUGIN_MAX_RESULTS) {
        return ZOVA_PLUGIN_INVALID_ARGUMENT;
    }

    const void *raw_data = NULL;
    const void *raw_query = NULL;
    int32_t status = plugin_service(host, connection, ZOVA_PLUGIN_SERVICE_DATA,
                                    sizeof(zova_plugin_data_service_v1), &raw_data);
    if (status == ZOVA_PLUGIN_OK) {
        status = plugin_service(host, connection, ZOVA_PLUGIN_SERVICE_QUERY,
                                sizeof(zova_plugin_query_service_v1), &raw_query);
    }
    if (status != ZOVA_PLUGIN_OK) return status;
    const zova_plugin_data_service_v1 *data = raw_data;
    const zova_plugin_query_service_v1 *query = raw_query;

    plugin_trace_t *trace = calloc(1, sizeof(*trace));
    if (!trace) return ZOVA_PLUGIN_OUT_OF_MEMORY;
    *out_cursor = trace;
    if (plugin_visit(trace, start_key) < 0) return ZOVA_PLUGIN_OUT_OF_MEMORY;

    zova_plugin_data_request_v1 request = {
        .struct_size = sizeof(request),
        .operation = ZOVA_PLUGIN_GRAPH_NEIGHBORS_KEYED,
        .name = {graph->bytes, graph->bytes_len},
        .direction = direction,
        .row_limit = PLUGIN_PAGE_ROWS,
        .byte_limit = PLUGIN_PAGE_BYTES,
        .row = plugin_neighbor_row,
        .user_data = trace,
    };
    if (edge_type_count == 1) {
        request.edge_type = edge_types[0];
    } else {
        request.ids = edge_types;
        request.id_count = (uint64_t)edge_type_count;
    }

    size_t level_start = 0;
    size_t level_end = 1;
    for (int64_t hop = 1; hop <= max_depth && level_start < level_end; hop++) {
        for (size_t first = level_start; first < level_end; first += PLUGIN_FRONTIER_BATCH) {
            size_t count = level_end - first;
            if (count > PLUGIN_FRONTIER_BATCH) count = PLUGIN_FRONTIER_BATCH;
            status = plugin_expand(data, connection, trace, &request, first, count);
            if (status != ZOVA_PLUGIN_OK) return status;
        }
        level_start = level_end;
        level_end = trace->key_count;
        trace->level = (int32_t)hop;
        status = plugin_hydrate(query, connection, trace, level_start, level_end - level_start);
        if (status != ZOVA_PLUGIN_OK) return status;
        /* Later levels sort after this one, so they cannot enter the result. */
        if (trace->count >= (size_t)max_results) break;
    }
    if (trace->count > 1) qsort(trace->rows, trace->count, sizeof(*trace->rows), plugin_row_compare);
    plugin_rows_truncate(trace, (size_t)max_results);
    return ZOVA_PLUGIN_OK;
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
    return ((const zova_plugin_operation_service_v1 *)raw)->register_operation(connection,
                                                                                &operation);
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
