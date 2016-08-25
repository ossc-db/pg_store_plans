/*-------------------------------------------------------------------------
 *
 * pgsp_json_text.h: Defenitions for text plan generator for pg_store_plan.
 *
 * Copyright (c) 2012-2015, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plan/pgsp_json_text.h
 *
 *-------------------------------------------------------------------------
 */

typedef struct
{
	NodeTag nodetag;
	const char *node_type;
	const char *operation;
	const char *subplan_name;

	const char *scan_dir;
	const char *index_name;
	const char *obj_name;
	const char *schema_name;

	const char *filter;
	const char *join_filter;
	const char *rows_removed_by_filter;
	const char *alias;
	StringInfo output;
	const char *func_call;
	const char *sort_method;
	StringInfo sort_key;
	const char *index_cond;
	const char *merge_cond;
	const char *hash_cond;
	const char *tid_cond;
	const char *recheck_cond;
	const char *hash_buckets;
	const char *hash_batches;
	const char *setopcommand;
	const char *join_type;
	const char *org_hash_batches;
	const char *org_hash_buckets;
	const char *peak_memory_usage;
	const char *startup_cost;
	const char *total_cost;
	const char *plan_rows;
	const char *plan_width;
	const char *sort_space_used;
	const char *sort_space_type;
	const char *actual_startup_time;
	const char *actual_total_time;
	const char *actual_rows;
	const char *actual_loops;
	const char *heap_fetches;
	const char *shared_hit_blks;
	const char *shared_read_blks;
	const char *shared_dirtied_blks;
	const char *shared_written_blks;
	const char *local_hit_blks;
	const char *local_read_blks;
	const char *local_dirtied_blks;
	const char *local_written_blks;
	const char *temp_read_blks;
	const char *temp_written_blks;
	const char *io_read_time;
	const char *io_write_time;
	const char *filter_removed;
	const char *idxrchk_removed;
	const char *trig_name;
	const char *trig_relation;
	const char *trig_time;
	const char *trig_calls;
	const char *plan_time;
	const char *exec_time;
	const char *exact_heap_blks;
	const char *lossy_heap_blks;
	const char *joinfilt_removed;
} node_vals;

#define SETTER(name) pgsp_node_set_##name

#define SETTERDECL(name) extern void SETTER(name)(node_vals *vals, const char *val)
#define DEFAULT_SETTER(name) \
	SETTERDECL(name) { vals->name = val;}

#define SQLQUOTE_SETTER(name) \
	SETTERDECL(name) { vals->name = quote_identifier(val);}

#define LIST_SETTER(name) \
	SETTERDECL(name) { \
		if (!vals->name)\
		{ \
			vals->name = makeStringInfo(); \
			appendStringInfoString(vals->name, val); \
		} \
		else \
		{ \
			appendStringInfoString(vals->name, ", "); \
			appendStringInfoString(vals->name, val); \
		} \
	}\

#define CONVERSION_SETTER(name, converter) \
	SETTERDECL(name) { vals->name = converter(val, PGSP_JSON_TEXTIZE);}

extern char *pgsp_json_textize(char *json);

/* Prototypes for setter for node_vals */
SETTERDECL(node_type);
SETTERDECL(scan_dir);
SETTERDECL(obj_name);
SETTERDECL(schema_name);
SETTERDECL(alias);
SETTERDECL(output);
SETTERDECL(strategy);
SETTERDECL(join_type);
SETTERDECL(setopcommand);
SETTERDECL(sort_method);
SETTERDECL(sort_key);
SETTERDECL(index_name);
SETTERDECL(startup_cost);
SETTERDECL(total_cost);
SETTERDECL(plan_rows);
SETTERDECL(plan_width);
SETTERDECL(sort_space_used);
SETTERDECL(sort_space_type);
SETTERDECL(filter);
SETTERDECL(join_filter);
SETTERDECL(func_call);
SETTERDECL(operation);
SETTERDECL(subplan_name);
SETTERDECL(index_cond);
SETTERDECL(hash_cond);
SETTERDECL(merge_cond);
SETTERDECL(tid_cond);
SETTERDECL(recheck_cond);
SETTERDECL(hash_buckets);
SETTERDECL(hash_batches);
SETTERDECL(org_hash_batches);
SETTERDECL(org_hash_buckets);
SETTERDECL(peak_memory_usage);
SETTERDECL(filter_removed);
SETTERDECL(idxrchk_removed);
SETTERDECL(actual_startup_time);
SETTERDECL(actual_total_time);
SETTERDECL(actual_rows);
SETTERDECL(actual_loops);
SETTERDECL(heap_fetches);
SETTERDECL(shared_hit_blks);
SETTERDECL(shared_read_blks);
SETTERDECL(shared_dirtied_blks);
SETTERDECL(shared_written_blks);
SETTERDECL(local_hit_blks);
SETTERDECL(local_read_blks);
SETTERDECL(local_dirtied_blks);
SETTERDECL(local_written_blks);
SETTERDECL(temp_read_blks);
SETTERDECL(temp_written_blks);
SETTERDECL(io_read_time);
SETTERDECL(io_write_time);
SETTERDECL(trig_name);
SETTERDECL(trig_relation);
SETTERDECL(trig_time);
SETTERDECL(trig_calls);
SETTERDECL(plan_time);
SETTERDECL(exec_time);
SETTERDECL(exact_heap_blks);
SETTERDECL(lossy_heap_blks);
SETTERDECL(joinfilt_removed);
