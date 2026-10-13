#ifndef CBM_ZOVA_PLUGIN_H
#define CBM_ZOVA_PLUGIN_H

/* Statically linked Zova plugin "cbm". Registers the read-only table operation
 *
 *   zova_cbm_trace_path(graph, start_key, direction, edge_types, max_depth, max_results)
 *
 * which walks the workspace graph breadth-first from one node key and returns
 * the reached cbm_nodes_v1 rows with their shortest hop. direction is
 * 'outbound' or 'inbound'; edge_types is a comma-separated list of 1..16 names.
 * Rows are ordered like cbm_store_bfs: hop, file_path, qualified_name, label,
 * start_line, end_line, node_key.
 *
 * Not installed in user databases: a database with the plugin installed can
 * only be opened by a host that links it. */
struct zova_plugin_descriptor_v1;

/* NULL when the build has no Zova. */
const struct zova_plugin_descriptor_v1 *cbm_zova_plugin_descriptor(void);

#endif /* CBM_ZOVA_PLUGIN_H */
