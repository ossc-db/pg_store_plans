#include "postgres.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/bitmapset.h"
#include "utils/json.h"
#include "utils/jsonapi.h"
#include "utils/builtins.h"

#include "pgsp_json_text.h"
#include "pgsp_json_int.h"

static void clear_nodeval(node_vals *vals);
static void print_current_node(pgspParserContext *ctx);
static void print_current_trig_node(pgspParserContext *ctx);
static void print_prop(StringInfo s, char *prepstr,
					   const char *prop, int leve, int exind);
static void print_prop_if_exists(StringInfo s, char *prepstr,
								 const char *prop, int leve, int exind);
static void print_prop_if_nz(StringInfo s, char *prepstr,
							 const char *prop, int leve, int exind);
static void json_text_objstart(void *state);
static void json_text_objend(void *state);
static void json_text_ofstart(void *state, char *fname, bool isnull);
static void json_text_scalar(void *state, char *token, JsonTokenType tokentype);

/* Parser callbacks for plan textization */
SETTERDECL(node_type)
{
	word_table *p;

	vals->node_type = val;
	vals->nodetag = T_Invalid;

	p = search_word_table(nodetypes, val, PGSP_JSON_TEXTIZE);
	if (p)
	{
		vals->node_type = (p->textname ? p->textname : p->longname);
		vals->nodetag = p->tag;
	}
}

SETTERDECL(output)
{
	if(!vals->output)
	{
		vals->output = makeStringInfo();
		appendStringInfoString(vals->output, val);
	}
	else
	{
		appendStringInfoString(vals->output, ", ");
		appendStringInfoString(vals->output, val);
	}
}
SETTERDECL(strategy)
{
	word_table *p;
	
	p = search_word_table(strategies, val, PGSP_JSON_TEXTIZE);

	switch (vals->nodetag)
	{
	case T_Agg:
		switch (p->tag)
		{
		case S_Hashed:
			vals->node_type = "HashAggregate"; break;
		case S_Sorted:
			vals->node_type = "GroupAggregate"; break;
		default:
			break;
		}
		break;

	case T_SetOp:
		if (p->tag == S_Hashed)
			vals->node_type = "HashSetOp";
		break;

	default:
		break;
	}
}
CONVERSION_SETTER(scan_dir, conv_scandir);
SQLQUOTE_SETTER(obj_name);
SQLQUOTE_SETTER(alias);
SQLQUOTE_SETTER(schema_name);
DEFAULT_SETTER(merge_cond);
CONVERSION_SETTER(join_type, conv_jointype);
CONVERSION_SETTER(setopcommand, conv_setsetopcommand);
CONVERSION_SETTER(sort_method, conv_sortmethod);
DEFAULT_SETTER(sort_key);
SQLQUOTE_SETTER(index_name);
DEFAULT_SETTER(startup_cost);
DEFAULT_SETTER(total_cost);
DEFAULT_SETTER(plan_rows);
DEFAULT_SETTER(plan_width);
DEFAULT_SETTER(sort_space_used);
CONVERSION_SETTER(sort_space_type, conv_sortspacetype);
DEFAULT_SETTER(filter);
DEFAULT_SETTER(join_filter);
DEFAULT_SETTER(func_call);
DEFAULT_SETTER(index_cond);
DEFAULT_SETTER(recheck_cond);
CONVERSION_SETTER(operation, conv_operation);
DEFAULT_SETTER(subplan_name);
DEFAULT_SETTER(hash_cond);
DEFAULT_SETTER(tid_cond);
DEFAULT_SETTER(filter_removed);
DEFAULT_SETTER(idxrchk_removed);
DEFAULT_SETTER(peak_memory_usage);
DEFAULT_SETTER(org_hash_batches);
DEFAULT_SETTER(hash_batches);
DEFAULT_SETTER(hash_buckets);
DEFAULT_SETTER(actual_startup_time);
DEFAULT_SETTER(actual_total_time);
DEFAULT_SETTER(actual_rows);
DEFAULT_SETTER(actual_loops);
DEFAULT_SETTER(heap_fetches);
DEFAULT_SETTER(shared_hit_blks);
DEFAULT_SETTER(shared_read_blks);
DEFAULT_SETTER(shared_dirtied_blks);
DEFAULT_SETTER(shared_written_blks);
DEFAULT_SETTER(local_hit_blks);
DEFAULT_SETTER(local_read_blks);
DEFAULT_SETTER(local_dirtied_blks);
DEFAULT_SETTER(local_written_blks);
DEFAULT_SETTER(temp_read_blks);
DEFAULT_SETTER(temp_written_blks);
DEFAULT_SETTER(io_read_time);
DEFAULT_SETTER(io_write_time);
SQLQUOTE_SETTER(trig_name);
SQLQUOTE_SETTER(trig_relation);
DEFAULT_SETTER(trig_time);
DEFAULT_SETTER(trig_calls);

#define ISZERO(s) (!s || strcmp(s, "0") == 0 || strcmp(s, "0.000") == 0 )
#define HASSTRING(s) (s && strlen(s) > 0)
#define TEXT_LEVEL_STEP 6
#define TEXT_INDENT_OFFSET 2
#define TEXT_INDENT_BASE(l, e)											\
	((l < 2) ? 0 : (TEXT_LEVEL_STEP * (l - 2) + TEXT_INDENT_OFFSET) + e)
#define TEXT_INDENT_DETAILS(l, e)					\
	(TEXT_INDENT_BASE(l, e) + ((l < 2) ? 2 : 6))

static void
print_obj_name(pgspParserContext *ctx)
{
	node_vals *v = ctx->nodevals;
	StringInfo s = ctx->dest;
	bool on_written = false;

	if (HASSTRING(v->obj_name))
	{
		on_written = true;
		appendStringInfoString(s, " on ");
		if (HASSTRING(v->schema_name))
		{
			appendStringInfoString(s, v->schema_name);
			appendStringInfoChar(s, '.');
		}
		appendStringInfoString(s, v->obj_name);
	}
	if (HASSTRING(v->alias) &&
		(!HASSTRING(v->obj_name) || strcmp(v->obj_name, v->alias) != 0))
	{
		if (!on_written)
			appendStringInfoString(s, " on ");
		else
			appendStringInfoChar(s, ' ');
		appendStringInfoString(s, v->alias);
	}
}


static void
print_prop(StringInfo s, char *prepstr,
		   const char *prop, int level, int exind)
{
	if (level > 0)
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
	}
	appendStringInfoString(s, prepstr);
	appendStringInfoString(s, prop);
}

static void
print_prop_if_exists(StringInfo s, char *prepstr,
					 const char *prop, int level, int exind)
{
	if (HASSTRING(prop))
		print_prop(s, prepstr, prop, level, exind);
}

static void
print_prop_if_nz(StringInfo s, char *prepstr,
				 const char *prop, int level, int exind)
{
	if (!ISZERO(prop))
		print_prop(s, prepstr, prop, level, exind);
}

static void 
print_current_node(pgspParserContext *ctx)
{
	node_vals *v = ctx->nodevals;
	StringInfo s = ctx->dest;
	int level = ctx->level - 1;
	bool comma = false;
	int exind = 0;

	if (v->node_type == T_Invalid)
		return;

	if (s->len > 0)
		appendStringInfoString(s, "\n");
	appendStringInfoSpaces(s, TEXT_INDENT_BASE(level, exind));

	if (HASSTRING(v->subplan_name))
	{
		appendStringInfoString(s, v->subplan_name);
		appendStringInfoString(s, "\n");
		exind = 2;
		appendStringInfoSpaces(s, TEXT_INDENT_BASE(level, exind));
	}
	
	if (level > 1)
		appendStringInfoString(s, "->  ");

	switch (v->nodetag)
	{
	case T_ModifyTable:
	case T_SeqScan:
	case T_BitmapHeapScan:
	case T_TidScan:
	case T_SubqueryScan:
	case T_FunctionScan:
	case T_ValuesScan:
	case T_CteScan:
	case T_WorkTableScan:
	case T_ForeignScan:
		if (v->nodetag == T_ModifyTable)
			appendStringInfoString(s, v->operation);
		else
			appendStringInfoString(s, v->node_type);

		print_obj_name(ctx);
		break;

	case T_IndexScan:
	case T_IndexOnlyScan:
	case T_BitmapIndexScan:
		appendStringInfoString(s, v->node_type);
		print_prop_if_exists(s, " ", v->scan_dir, 0, 0);
		print_prop_if_exists(s, " using ", v->index_name, 0, 0);
		print_obj_name(ctx);
		break;

	case T_NestLoop:
	case T_MergeJoin:
	case T_HashJoin:
		appendStringInfoString(s, v->node_type);
		if (v->join_type && strcmp(v->join_type, "Inner") != 0)
		{
			appendStringInfoChar(s, ' ');
			appendStringInfoString(s, v->join_type);
		}
		if (v->nodetag != T_NestLoop)
			appendStringInfoString(s, " Join");
		break;

	case T_SetOp:
		appendStringInfoString(s, v->node_type);
		print_prop_if_exists(s, " ", v->setopcommand, 0, 0);
		break;

	default:
		appendStringInfoString(s, v->node_type);
		break;
	}
	
	if (!ISZERO(v->startup_cost) &&
		!ISZERO(v->total_cost) &&
		HASSTRING(v->plan_rows) &&
		HASSTRING(v->plan_width))
	{
		appendStringInfoString(s, "  (cost=");
		appendStringInfoString(s, v->startup_cost);
		appendStringInfoString(s, "..");
		appendStringInfoString(s, v->total_cost);
		appendStringInfoString(s, " rows=");
		appendStringInfoString(s, v->plan_rows);
		appendStringInfoString(s, " width=");
		appendStringInfoString(s, v->plan_width);
		appendStringInfoString(s, ")");
	}

	if (HASSTRING(v->actual_startup_time) &&
		HASSTRING(v->actual_total_time) &&
		HASSTRING(v->actual_rows) &&
		HASSTRING(v->actual_loops))
	{
		if (ISZERO(v->actual_loops))
		{
			appendStringInfoString(s, " (never executed)");
		}
		else
		{
			appendStringInfoString(s, " (actual time=");
			appendStringInfoString(s, v->actual_startup_time);
			appendStringInfoString(s, "..");
			appendStringInfoString(s, v->actual_total_time);
			appendStringInfoString(s, " rows=");
			appendStringInfoString(s, v->actual_rows);
			appendStringInfoString(s, " loops=");
			appendStringInfoString(s, v->actual_loops);
			appendStringInfoString(s, ")");
		}
	}

	if (v->output)
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "Output: ");
		appendStringInfoString(s, v->output->data);
	}
	print_prop_if_exists(s, "Merge Cond: ", v->merge_cond, level, exind);
	print_prop_if_exists(s, "Hash Cond: " , v->hash_cond, level, exind);
	print_prop_if_exists(s, "Tid Cond: " , v->tid_cond, level, exind);
	print_prop_if_exists(s, "Join Filter: " , v->join_filter, level, exind);
	print_prop_if_exists(s, "Index Cond: " , v->index_cond, level, exind);
	print_prop_if_exists(s, "Recheck Cond: ", v->recheck_cond, level, exind);
	print_prop_if_exists(s, "Sort Key: ", v->sort_key, level, exind);

	if (HASSTRING(v->sort_method))
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "Sort Method: ");
		appendStringInfoString(s, v->sort_method);

		if (HASSTRING(v->sort_space_type) &&
			HASSTRING(v->sort_space_used))
		{
			appendStringInfoString(s, "  ");
			appendStringInfoString(s, v->sort_space_type);
			appendStringInfoString(s, ": ");
			appendStringInfoString(s, v->sort_space_used);
			appendStringInfoString(s, "kB");
		}
	}

	print_prop_if_exists(s, "Function Call: ", v->func_call, level, exind);
	print_prop_if_exists(s, "Filter: ", v->filter, level, exind);
	print_prop_if_nz(s, "Rows Removed by Filter: ",
						 v->filter_removed, level, exind);
	print_prop_if_nz(s, "Rows Removed by Index Recheck: ",
						 v->idxrchk_removed, level, exind);
	
	if (!ISZERO(v->hash_buckets))
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "Buckets: ");
		appendStringInfoString(s, v->hash_buckets);
		if (!ISZERO(v->hash_batches))
		{
			appendStringInfoString(s, "  Batches: ");
			appendStringInfoString(s, v->hash_batches);
			if (v->org_hash_batches &&
				strcmp(v->hash_batches, v->org_hash_batches) != 0)
			{
				appendStringInfoString(s, " (originally ");
				appendStringInfoString(s, v->org_hash_batches);
				appendStringInfoChar(s, ')');
			}
		}
		if (!ISZERO(v->peak_memory_usage))
		{
			appendStringInfoString(s, "  Memory Usage: ");
			appendStringInfoString(s, v->peak_memory_usage);
			appendStringInfoString(s, "kB");
		}
	}

	print_prop_if_exists(s, "Heap Fetches: ", v->heap_fetches, level, exind);

	if (!ISZERO(v->shared_hit_blks) ||
		!ISZERO(v->shared_read_blks) ||
		!ISZERO(v->shared_dirtied_blks) ||
		!ISZERO(v->shared_written_blks))
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "Buffers: shared");

		if (!ISZERO(v->shared_hit_blks))
		{
			appendStringInfoString(s, " hit=");
			appendStringInfoString(s, v->shared_hit_blks);
			comma =true;
		}
		if (!ISZERO(v->shared_read_blks))
		{
			appendStringInfoString(s, " read=");
			appendStringInfoString(s, v->shared_read_blks);
			comma =true;
		}
		if (!ISZERO(v->shared_dirtied_blks))
		{
			appendStringInfoString(s, " dirtied=");
			appendStringInfoString(s, v->shared_dirtied_blks);
			comma =true;
		}
		if (!ISZERO(v->shared_written_blks))
		{
			appendStringInfoString(s, " written=");
			appendStringInfoString(s, v->shared_written_blks);
			comma =true;
		}
	}
	if (!ISZERO(v->local_hit_blks) ||
		!ISZERO(v->local_read_blks) ||
		!ISZERO(v->local_dirtied_blks) ||
		!ISZERO(v->local_written_blks))
	{
		if (comma)
			appendStringInfoString(s, ", ");
		else
		{
			appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
			appendStringInfoString(s, "Buffers: ");
		}

		appendStringInfoString(s, "local");
		if (!ISZERO(v->local_hit_blks))
		{
			appendStringInfoString(s, " hit=");
			appendStringInfoString(s, v->local_hit_blks);
			comma =true;
		}
		if (!ISZERO(v->local_read_blks))
		{
			appendStringInfoString(s, " read=");
			appendStringInfoString(s, v->local_read_blks);
			comma =true;
		}
		if (!ISZERO(v->local_dirtied_blks))
		{
			appendStringInfoString(s, " dirtied=");
			appendStringInfoString(s, v->local_dirtied_blks);
			comma =true;
		}
		if (!ISZERO(v->local_written_blks))
		{
			appendStringInfoString(s, " written=");
			appendStringInfoString(s, v->local_written_blks);
			comma =true;
		}
	}
	if (!ISZERO(v->temp_read_blks) ||
		!ISZERO(v->temp_written_blks))
	{
		if (comma)
			appendStringInfoString(s, ", ");
		else
		{
			appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
			appendStringInfoString(s, "Buffers: ");
		}

		appendStringInfoString(s, "temp");
		if (!ISZERO(v->temp_read_blks))
		{
			appendStringInfoString(s, " read=");
			appendStringInfoString(s, v->temp_read_blks);
			comma =true;
		}
		if (!ISZERO(v->temp_written_blks))
		{
			appendStringInfoString(s, " written=");
			appendStringInfoString(s, v->temp_written_blks);
			comma =true;
		}
	}
}

static void
print_current_trig_node(pgspParserContext *ctx)
{
	node_vals *v = ctx->nodevals;
	StringInfo s = ctx->dest;

	if (HASSTRING(v->trig_name) && !ISZERO(v->trig_time))
	{
		if (s->len > 0)
			appendStringInfoString(s, "\n");
		appendStringInfoString(s, "Trigger ");
		appendStringInfoString(s, v->trig_name);
		appendStringInfoString(s, ": time=");
		appendStringInfoString(s, v->trig_time);
		appendStringInfoString(s, " calls=");
		appendStringInfoString(s, v->trig_calls);
	}
}


static void
clear_nodeval(node_vals *vals)
{
	memset(vals, 0, sizeof(node_vals));
}

static void
json_text_objstart(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	clear_nodeval(ctx->nodevals);
	ctx->level++;
}
static void
json_text_objend(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	if (ctx->processing == P_Plan)
		print_current_node(ctx);
	else
		print_current_trig_node(ctx);

	clear_nodeval(ctx->nodevals);
	ctx->last_elem_is_object = true;
	ctx->level--;
}

static void
json_text_ofstart(void *state, char *fname, bool isnull)
{
	word_table *p;
	pgspParserContext *ctx = (pgspParserContext *)state;

	ctx->setter = NULL;
	p = search_word_table(propfields, fname, PGSP_JSON_TEXTIZE);

	if (!p)
	{
		ereport(DEBUG1,
				(errmsg("Short JSON parser encoutered unknown field name: \"%s\", skipped.", fname),
				 errdetail_log("INPUT: \"%s\"", ctx->org_string)));
	}		
	if (p && (p->tag == P_Plan || p->tag == P_Plans))
	{
		print_current_node(ctx);
		clear_nodeval(ctx->nodevals);
	}

	if (p && (p->tag == P_Plan || p->tag == P_Triggers))
		ctx->processing = p->tag;

	ctx->setter = (p ? p->setter : NULL);
}

static void
json_text_scalar(void *state, char *token, JsonTokenType tokentype)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (ctx->setter)
		ctx->setter(ctx->nodevals, token);
}

char *
pgsp_json_textize(char *json)
{
	JsonLexContext lex;
	JsonSemAction sem;
	pgspParserContext	ctx;

	init_json_lex_context(&lex, json);
	init_parser_context(&ctx, PGSP_JSON_TEXTIZE, json, NULL, 0);

	ctx.nodevals = (node_vals*)palloc0(sizeof(node_vals));

	sem.semstate = (void*)&ctx;
	sem.object_start       = json_text_objstart;
	sem.object_end         = json_text_objend;
	sem.array_start        = NULL;
	sem.array_end          = NULL;
	sem.object_field_start = json_text_ofstart;
	sem.object_field_end   = NULL;
	sem.array_element_start= NULL;
	sem.array_element_end  = NULL;
	sem.scalar             = json_text_scalar;


	if (!run_pg_parse_json(&lex, &sem))
	{
		if (ctx.nodevals->node_type)
			print_current_node(&ctx);

		if (ctx.dest->len > 0 &&
			ctx.dest->data[ctx.dest->len - 1] != '\n')
			appendStringInfoChar(ctx.dest, '\n');
		
		if (ctx.dest->len == 0)
			appendStringInfoString(ctx.dest, "<Input was not JSON>");
		else
			appendStringInfoString(ctx.dest, "<truncated>");
	}

	pfree(ctx.nodevals);

	return ctx.dest->data;
}
