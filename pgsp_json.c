/*-------------------------------------------------------------------------
 *
 * pgsp_json.c: Plan handler for JSON/XML/YAML style plans
 *
 * Copyright (c) 2012-2021, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plans/pgsp_json.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/bitmapset.h"
#include "parser/scanner.h"
#include "parser/gram.h"
#include "utils/xml.h"
#include "utils/json.h"
#if PG_VERSION_NUM < 130000
#include "utils/jsonapi.h"
#else
#include "common/jsonapi.h"
#endif
#include "pgsp_json.h"
#include "pgsp_json_int.h"

#define INDENT_STEP 2


void normalize_expr(char *expr, bool preserve_space);
static const char *converter_core(word_table *tbl,
						 const char *src, pgsp_parser_mode mode);

static void json_objstart(void *state);
static void json_objend(void *state);
static void json_arrstart(void *state);
static void json_arrend(void *state);
static void json_ofstart(void *state, char *fname, bool isnull);
static void json_aestart(void *state, bool isnull);
static void json_scalar(void *state, char *token, JsonTokenType tokentype);

static void yaml_objstart(void *state);
static void yaml_objend(void *state);
static void yaml_arrstart(void *state);
static void yaml_arrend(void *state);
static void yaml_ofstart(void *state, char *fname, bool isnull);
static void yaml_aestart(void *state, bool isnull);
static void yaml_scalar(void *state, char *token, JsonTokenType tokentype);

static void adjust_wbuf(pgspParserContext *ctx, int len);
static char *hyphenate_words(pgspParserContext *ctx, char *src);
static void xml_objstart(void *state);
static void xml_objend(void *state);
static void xml_arrend(void *state);
static void xml_ofstart(void *state, char *fname, bool isnull);
static void xml_ofend(void *state, char *fname, bool isnull);
static void xml_aestart(void *state, bool isnull);
static void xml_aeend(void *state, bool isnull);
static void xml_scalar(void *state, char *token, JsonTokenType tokentype) ;

static void init_json_semaction(JsonSemAction *sem,
										  pgspParserContext *ctx);

word_table propfields[] =
{
	{P_NodeType,		"t" ,"Node Type",			NULL, true,  conv_nodetype,		SETTER(node_type)},
	{P_RelationShip,	"h" ,"Parent Relationship",	NULL, true,  conv_relasionship,	NULL},
	{P_RelationName,	"n" ,"Relation Name",		NULL, true,  NULL,				SETTER(obj_name)},
	{P_FunctioName,		"f" ,"Function Name",		NULL, true,  NULL,				SETTER(obj_name)},
	{P_IndexName,		"i" ,"Index Name",			NULL, true,  NULL,				SETTER(index_name)},
	{P_CTEName,			"c" ,"CTE Name",			NULL, true,  NULL,				SETTER(obj_name)},
	{P_TrgRelation,		"w" ,"Relation",			NULL, true,  NULL,				SETTER(trig_relation)},
	{P_Schema,			"s" ,"Schema",				NULL, true,  NULL,				SETTER(schema_name)},
	{P_Alias,			"a" ,"Alias",				NULL, true,  NULL,				SETTER(alias)},
	{P_Output,			"o" ,"Output",				NULL, true,  conv_expression, 	SETTER(output)},
	{P_ScanDir,			"d" ,"Scan Direction",		NULL, true,  conv_scandir,		SETTER(scan_dir)},
	{P_MergeCond,		"m" ,"Merge Cond",			NULL, true,  conv_expression,	SETTER(merge_cond)},
	{P_Strategy,		"g" ,"Strategy",			NULL, true,  conv_strategy,		SETTER(strategy)},
	{P_JoinType,		"j" ,"Join Type",			NULL, true,  conv_jointype,		SETTER(join_type)},
	{P_SortMethod,		"e" ,"Sort Method",			NULL, true,  conv_sortmethod,	SETTER(sort_method)},
	{P_SortKey,			"k" ,"Sort Key",			NULL, true,  conv_expression,	SETTER(sort_key)},
	{P_Filter,			"5" ,"Filter",				NULL, true,  conv_expression,	SETTER(filter)},
	{P_JoinFilter,		"6" ,"Join Filter",			NULL, true,  conv_expression,	SETTER(join_filter)},
	{P_HashCond,		"7" ,"Hash Cond",			NULL, true,  conv_expression,	SETTER(hash_cond)},
	{P_IndexCond,		"8" ,"Index Cond",			NULL, true,  conv_expression,	SETTER(index_cond)},
	{P_TidCond,			"9" ,"TID Cond",			NULL, true,  conv_expression,	SETTER(tid_cond)},
	{P_RecheckCond,		"0" ,"Recheck Cond",		NULL, true,  conv_expression,	SETTER(recheck_cond)},
	{P_Operation,		"!" ,"Operation",			NULL, true,  conv_operation,	SETTER(operation)},
	{P_SubplanName,		"q" ,"Subplan Name",		NULL, true,  NULL,				SETTER(subplan_name)},
	{P_Command,			"b" ,"Command",				NULL, true,  conv_setsetopcommand,SETTER(setopcommand)},
	{P_Triggers,		"r" ,"Triggers",			NULL, true,  NULL,				NULL},
	{P_Trigger,			"u" ,"Trigger",				NULL, true,  NULL,				SETTER(node_type)},
	{P_TriggerName,		"v" ,"Trigger Name",		NULL, true,  NULL,				SETTER(trig_name)},
	{P_ConstraintName,	"x" ,"Constraint Name",		NULL, true,  NULL,				NULL},
	{P_Plans,			"l" ,"Plans",				NULL, true,  NULL,				NULL},
	{P_Plan,			"p" ,"Plan",				NULL, true,  NULL,				NULL},
	{P_GroupKey,    	"-" ,"Group Key",			NULL, true,  NULL,				SETTER(group_key)},
	{P_GroupSets,    	"=" ,"Grouping Sets",		NULL, true,  NULL,				NULL},
	{P_GroupKeys,    	"\\" ,"Group Keys",			NULL, true,  NULL,				SETTER(group_key)},

	{P_HashKeys,    	"~" ,"Hash Keys",			NULL, true,  NULL,				SETTER(hash_key)},
	{P_HashKey,    		"|" ,"Hash Key",			NULL, true,  NULL,				SETTER(hash_key)},

	{P_Parallel,    	"`" ,"Parallel Aware",		NULL, true,  NULL,				SETTER(parallel_aware)},
	{P_PartialMode,    	">" ,"Partial Mode",		NULL, true,  conv_partialmode,SETTER(partial_mode)},
	{P_WorkersPlanned, 	"{" ,"Workers Planned",		NULL, true,  NULL,				SETTER(workers_planned)},
	{P_WorkersLaunched, "}" ,"Workers Launched",	NULL, true,  NULL,				SETTER(workers_launched)},
	{P_InnerUnique,		"?" ,"Inner Unique",		NULL, true,  NULL,				SETTER(inner_unique)},
	{P_AsyncCapable,	"ac", "Async Capable",		NULL, true,  NULL,				SETTER(async_capable)},

	/* Values of these properties are ignored on normalization */
	{P_FunctionCall,	"y" ,"Function Call",		NULL, false, NULL,				SETTER(func_call)},
	{P_StartupCost,		"1" ,"Startup Cost",		NULL, false, NULL,				SETTER(startup_cost)},
	{P_TotalCost,		"2" ,"Total Cost",			NULL, false, NULL,				SETTER(total_cost)},
	{P_PlanRows,		"3" ,"Plan Rows",			NULL, false, NULL,				SETTER(plan_rows)},
	{P_PlanWidth,		"4" ,"Plan Width",			NULL, false, NULL,				SETTER(plan_width)},
	{P_ActualStartupTime,"A","Actual Startup Time",	NULL, false, NULL,				SETTER(actual_startup_time)},
	{P_ActualTotalTime, "B" ,"Actual Total Time",	NULL, false, NULL,				SETTER(actual_total_time)},
	{P_ActualRows,		"C" ,"Actual Rows",			NULL, false, NULL,				SETTER(actual_rows)},
	{P_ActualLoops,		"D" ,"Actual Loops",		NULL, false, NULL,				SETTER(actual_loops)},
	{P_HeapFetches,		"E" ,"Heap Fetches",		NULL, false, NULL,				SETTER(heap_fetches)},
	{P_SharedHitBlks,	"F" ,"Shared Hit Blocks",	NULL, false, NULL,				SETTER(shared_hit_blks)},
	{P_SharedReadBlks,	"G" ,"Shared Read Blocks", 	NULL, false, NULL,				SETTER(shared_read_blks)},
	{P_SharedDirtiedBlks,"H","Shared Dirtied Blocks",NULL,false, NULL,				SETTER(shared_dirtied_blks)},
	{P_SharedWrittenBlks,"I","Shared Written Blocks",NULL,false, NULL,				SETTER(shared_written_blks)},
	{P_LocalHitBlks,	"J" ,"Local Hit Blocks",	NULL, false, NULL,				SETTER(local_hit_blks)},
	{P_LocalReadBlks,	"K" ,"Local Read Blocks",	NULL, false, NULL,				SETTER(local_read_blks)},
	{P_LocalDirtiedBlks,"L" ,"Local Dirtied Blocks",NULL, false, NULL,				SETTER(local_dirtied_blks)},
	{P_LocalWrittenBlks,"M" ,"Local Written Blocks",NULL, false, NULL,				SETTER(local_written_blks)},
	{P_TempReadBlks,	"N" ,"Temp Read Blocks",	NULL, false, NULL,				SETTER(temp_read_blks)},
	{P_TempWrittenBlks, "O" ,"Temp Written Blocks",	NULL, false, NULL,				SETTER(temp_written_blks)},
	{P_IOReadTime,		"P" ,"I/O Read Time",		NULL, false, NULL,				SETTER(io_read_time)},
	{P_IOWwriteTime,	"Q" ,"I/O Write Time",		NULL, false, NULL,				SETTER(io_write_time)},
	{P_SortSpaceUsed,	"R" ,"Sort Space Used",		NULL, false, NULL,				SETTER(sort_space_used)},
	{P_SortSpaceType,	"S" ,"Sort Space Type",		NULL, false, conv_sortspacetype,SETTER(sort_space_type)},
	{P_PeakMemoryUsage,	"T" ,"Peak Memory Usage",	NULL, false, NULL,				SETTER(peak_memory_usage)},
	{P_OrgHashBatches,	"U","Original Hash Batches",NULL, false, NULL,				SETTER(org_hash_batches)},
	{P_OrgHashBuckets,	"*","Original Hash Buckets",NULL, false, NULL,				SETTER(org_hash_buckets)},
	{P_HashBatches,		"V" ,"Hash Batches",		NULL, false, NULL,				SETTER(hash_batches)},
	{P_HashBuckets,		"W" ,"Hash Buckets",		NULL, false, NULL,				SETTER(hash_buckets)},
	{P_RowsFilterRmvd,	"X" ,"Rows Removed by Filter",NULL,false,NULL,				SETTER(filter_removed)},
	{P_RowsIdxRchkRmvd, "Y" ,"Rows Removed by Index Recheck",NULL,false, NULL,		SETTER(idxrchk_removed)},
	{P_TrgTime,			"Z" ,"Time",				NULL, false,  NULL,				SETTER(trig_time)},
	{P_TrgCalls,		"z" ,"Calls",				NULL, false,  NULL,				SETTER(trig_calls)},
	{P_PlanTime,		"#" ,"Planning Time",		NULL, false,  NULL,				SETTER(plan_time)},
	{P_ExecTime,		"$" ,"Execution Time",		NULL, false,  NULL,				SETTER(exec_time)},
	{P_ExactHeapBlks,	"&" ,"Exact Heap Blocks",	NULL, false,  NULL,				SETTER(exact_heap_blks)},
	{P_LossyHeapBlks,	"(" ,"Lossy Heap Blocks",	NULL, false,  NULL,				SETTER(lossy_heap_blks)},
	{P_RowsJoinFltRemvd,")" ,"Rows Removed by Join Filter",	NULL, false,  NULL,		SETTER(joinfilt_removed)},
	{P_TargetTables,    "_" ,"Target Tables",		NULL, false,  NULL,				NULL},
	{P_ConfRes,			"%" ,"Conflict Resolution",	NULL, false,  NULL,			SETTER(conflict_resolution)},
	{P_ConfArbitIdx,    "@" ,"Conflict Arbiter Indexes",NULL, false,  NULL,			SETTER(conflict_arbiter_indexes)},
	{P_TuplesInserted,  "^" ,"Tuples Inserted",		NULL, false,  NULL,				SETTER(tuples_inserted)},
	{P_ConfTuples,		"+" ,"Conflicting Tuples",	NULL, false,  NULL,				SETTER(conflicting_tuples)},
	{P_SamplingMethod,  ":"  ,"Sampling Method" ,	NULL, false,  NULL,				SETTER(sampling_method)},
	{P_SamplingParams,  ";"  ,"Sampling Parameters" , NULL, false,  NULL,			SETTER(sampling_params)},
	{P_RepeatableSeed,  "<"  ,"Repeatable Seed" ,	NULL, false,  NULL,				SETTER(repeatable_seed)},
	{P_Workers,    		"[" ,"Workers",				NULL, false,  NULL,				NULL},
	{P_WorkerNumber,    "]" ,"Worker Number",		NULL, false,  NULL,				SETTER(worker_number)},
	{P_TableFuncName,   "aa" ,"Table Function Name",NULL, false,  NULL,			SETTER(table_func_name)},

	{P_PresortedKey,    "pk" ,"Presorted Key" 	   ,NULL, false,  NULL,			SETTER(presorted_key)},
	{P_FullsortGroups,  "fg" ,"Full-sort Groups"   ,NULL, false,  NULL,			NULL},
	{P_SortMethodsUsed, "su" ,"Sort Methods Used"  ,NULL, false,  NULL,			SETTER(sortmethod_used)},
	{P_SortSpaceMemory, "sm" ,"Sort Space Memory"  ,NULL, false,  NULL,			SETTER(sortspace_mem)},
	{P_GroupCount, 		"gc" ,"Group Count" 	   ,NULL, false,  NULL,			SETTER(group_count)},
	{P_AvgSortSpcUsed,  "as" ,"Average Sort Space Used",NULL, false,  NULL,		SETTER(avg_sortspc_used)},
	{P_PeakSortSpcUsed, "ps" ,"Peak Sort Space Used",NULL, false,  NULL,		SETTER(peak_sortspc_used)},
	{P_PreSortedGroups, "pg" ,"Pre-sorted Groups"  ,NULL, false,  NULL,			NULL},

	{P_Invalid, NULL, NULL, NULL, false, NULL, NULL}
};

word_table nodetypes[] =
{
	{T_Result,		"a" ,"Result",			NULL, false, NULL, NULL},
	{T_ModifyTable,	"b" ,"ModifyTable",	NULL, false, NULL, NULL},
	{T_Append,		"c" ,"Append",			NULL, false, NULL, NULL},
	{T_MergeAppend,	"d" ,"Merge Append",	NULL, false, NULL, NULL},
	{T_RecursiveUnion,"e" ,"Recursive Union",NULL, false, NULL, NULL},
	{T_BitmapAnd,	"f" ,"BitmapAnd",		NULL, false, NULL, NULL},
	{T_BitmapOr,	"g" ,"BitmapOr",		NULL, false, NULL, NULL},
	{T_Scan,		""  , "", "", false, NULL, NULL},
	{T_SeqScan,		"h" ,"Seq Scan",		NULL, false, NULL, NULL},
	{T_IndexScan,	"i" ,"Index Scan",		NULL, false, NULL, NULL},
	{T_IndexOnlyScan,"j","Index Only Scan",NULL, false, NULL, NULL},
	{T_BitmapIndexScan,"k" ,"Bitmap Index Scan", NULL, false, NULL, NULL},
	{T_BitmapHeapScan,"l" ,"Bitmap Heap Scan", NULL ,false, NULL, NULL},
	{T_TidScan,		"m" ,"Tid Scan",		NULL, false, NULL, NULL},
	{T_SubqueryScan,"n" ,"Subquery Scan",	NULL, false, NULL, NULL},
	{T_FunctionScan,"o" ,"Function Scan",	NULL, false, NULL, NULL},
	{T_ValuesScan,	"p" ,"Values Scan",	NULL, false, NULL, NULL},
	{T_CteScan,		"q" ,"CTE Scan",		NULL, false, NULL, NULL},
	{T_WorkTableScan,"r","WorkTable Scan",	NULL, false, NULL, NULL},
	{T_ForeignScan,	"s" , "Foreign Scan",	NULL, false, NULL, NULL},
	{T_Join,		""  ,   "",				NULL, false, NULL, NULL},
	{T_NestLoop,	"t" ,"Nested Loop",	NULL, false, NULL, NULL},
	{T_MergeJoin,	"u" ,"Merge Join",		"Merge", false, NULL, NULL},
	{T_HashJoin,	"v" ,"Hash Join",		"Hash", false, NULL, NULL},
	{T_Material,	"w" ,"Materialize",		NULL, false, NULL, NULL},
	{T_Sort,		"x" ,"Sort",			NULL, false, NULL, NULL},
	{T_Group,		"y" ,"Group",			NULL, false, NULL, NULL},
	{T_Agg,			"z" ,"Aggregate",		NULL, false, NULL, NULL},
	{T_WindowAgg,	"0" ,"WindowAgg",		NULL, false, NULL, NULL},
	{T_Unique,		"1" ,"Unique",			NULL, false, NULL, NULL},
	{T_Hash,		"2" ,"Hash",			NULL, false, NULL, NULL},
	{T_SetOp,		"3" ,"SetOp",			NULL, false, NULL, NULL},
	{T_LockRows,	"4" ,"LockRows",		NULL, false, NULL, NULL},
	{T_Limit,		"5" ,"Limit",			NULL, false, NULL, NULL},
#if PG_VERSION_NUM >= 90500
	{T_SampleScan,	"B" ,"Sample Scan",		NULL, false, NULL, NULL},
#endif
#if PG_VERSION_NUM >= 90600
	{T_Gather,		"6" ,"Gather",			NULL, false, NULL, NULL},
#endif
#if PG_VERSION_NUM >= 100000
	{T_ProjectSet,	"7" ,"ProjectSet",		NULL, false, NULL, NULL},
	{T_TableFuncScan,"8","Table Function Scan",	NULL, false, NULL, NULL},
	{T_NamedTuplestoreScan,"9","Named Tuplestore Scan",	NULL, false, NULL, NULL},
	{T_GatherMerge,	"A" ,"Gather Merge",	NULL, false, NULL, NULL},
#endif
#if PG_VERSION_NUM >= 130000
	{T_IncrementalSort,	"C" ,"Incremental Sort", NULL, false, NULL, NULL},
#endif
#if PG_VERSION_NUM >= 140000
	{T_TidRangeScan,"D", "Tid Range Scan",	NULL, false, NULL, NULL},
	{T_Memoize,		"E", "Memoize",			NULL, false, NULL, NULL},
#endif

	{T_Invalid,		NULL, NULL, NULL, false, NULL, NULL}
};

word_table directions[] =
{
	{T_Invalid,  "b" ,"Backward",	"Backward", false, NULL, NULL},
	{T_Invalid,  "n" ,"NoMovement","",			false, NULL, NULL},
	{T_Invalid,  "f" ,"Forward",	"",			false, NULL, NULL},
	{T_Invalid, NULL , NULL,		NULL,		false, NULL, NULL}
};

word_table relationships[] =
{
	{T_Invalid,  "o" ,"Outer", NULL, false, NULL, NULL},
	{T_Invalid,  "i" ,"Inner", NULL, false, NULL, NULL},
	{T_Invalid,  "s" ,"Subquery", NULL, false, NULL, NULL},
	{T_Invalid,  "m" ,"Member", NULL, false, NULL, NULL},
	{T_Invalid,  "I" ,"InitPlan", NULL, false, NULL, NULL},
	{T_Invalid,  "S" ,"SubPlan", NULL, false, NULL, NULL},
	{T_Invalid, NULL, NULL, NULL, false, NULL, NULL}
};

word_table strategies[] =
{
	{S_Plain,	"p" ,"Plain", NULL, false, NULL, NULL},
	{S_Sorted,	"s" ,"Sorted", NULL, false, NULL, NULL},
	{S_Hashed,	"h" ,"Hashed", NULL, false, NULL, NULL},
	{S_Mixed,	"m" ,"Mixed", NULL, false, NULL, NULL},
	{S_Invalid,	NULL, NULL, NULL, false, NULL, NULL}
};

word_table operations[] =
{
	{T_Invalid,  "i" ,"Insert", NULL, false, NULL, NULL},
	{T_Invalid,  "d" ,"Delete", NULL, false, NULL, NULL},
	{T_Invalid,  "u" ,"Update", NULL, false, NULL, NULL},
	{T_Invalid, NULL, NULL, NULL, false, NULL, NULL}
};

word_table jointypes[] =
{
	{T_Invalid,  "i" ,"Inner", NULL, false, NULL, NULL},
	{T_Invalid,  "l" ,"Left", NULL, false, NULL, NULL},
	{T_Invalid,  "f" ,"Full", NULL, false, NULL, NULL},
	{T_Invalid,  "r" ,"Right", NULL, false, NULL, NULL},
	{T_Invalid,  "s" ,"Semi", NULL, false, NULL, NULL},
	{T_Invalid,  "a" ,"Anti", NULL, false, NULL, NULL},
	{T_Invalid, NULL, NULL, NULL, false, NULL, NULL}
};

word_table setsetopcommands[] =
{
	{T_Invalid,  "i" ,"Intersect", NULL, false, NULL, NULL},
	{T_Invalid,  "I" ,"Intersect All", NULL, false, NULL, NULL},
	{T_Invalid,  "e" ,"Except", NULL, false, NULL, NULL},
	{T_Invalid,  "E" ,"Except All", NULL, false, NULL, NULL},
	{T_Invalid, NULL, NULL, NULL, false, NULL, NULL}
};

word_table sortmethods[] =
{
	{T_Invalid,  "h" ,"top-N heapsort", NULL, false, NULL, NULL},
	{T_Invalid,  "q" ,"quicksort", NULL, false, NULL, NULL},
	{T_Invalid,  "e" ,"external sort", NULL, false, NULL, NULL},
	{T_Invalid,  "E" ,"external merge", NULL, false, NULL, NULL},
	{T_Invalid,  "s" ,"still in progress", NULL, false, NULL, NULL},
	{T_Invalid, NULL, NULL, NULL, false, NULL, NULL}
};

word_table sortspacetype[] =
{
	{T_Invalid,  "d" ,"Disk",	NULL, false, NULL, NULL},
	{T_Invalid,  "m" ,"Memory",NULL, false, NULL, NULL},
	{T_Invalid, NULL, NULL, NULL, false, NULL, NULL}
};

word_table partialmode[] =
{
	{T_Invalid,  "p" ,"Partial",	NULL, false, NULL, NULL},
	{T_Invalid,  "f" ,"Finalize",NULL, false, NULL, NULL},
	{T_Invalid,  "s" ,"Simple",NULL, false, NULL, NULL},
	{T_Invalid, NULL, NULL, NULL, false, NULL, NULL}
};


word_table *
search_word_table(word_table *tbl, const char *word, int mode)
{
	word_table *p;

	bool longname =
		(mode == PGSP_JSON_SHORTEN || mode == PGSP_JSON_NORMALIZE);


	/*
	 * Use simple linear search. We can gain too small portion of the whole
	 * processing time using more 'clever' algorithms like b-tree or tries,
	 * which won't be worth the additional memory, complexity and
	 * initialization cost.
	 */
	for (p = tbl ; p->longname ; p++)
	{
		if (strcmp(longname ? p->longname: p->shortname, word) == 0)
			break;
	}

	if (p->longname == NULL && mode == PGSP_JSON_TEXTIZE)
	{
		/* Fallback to long json prop name */
		for (p = tbl ; p->longname ; p++)
			if (strcmp(p->longname, word) == 0)
				break;
	}

	return (p->longname ? p : NULL);
}


const char *
converter_core(word_table *tbl,
			   const char *src, pgsp_parser_mode mode)
{
	word_table *p;
	char *ret;

	p = search_word_table(tbl, src, mode);

	if (!p) return src;

	ret = p->shortname;
	switch(mode)
	{
		case PGSP_JSON_SHORTEN:
		case PGSP_JSON_NORMALIZE:
			ret = p->shortname;
			break;
		case PGSP_JSON_INFLATE:
		case PGSP_JSON_YAMLIZE:
		case PGSP_JSON_XMLIZE:
			ret = p->longname;
			break;
		case PGSP_JSON_TEXTIZE:
			if(p->textname)
				ret = p->textname;
			else
				ret = p->longname;
			break;
		default:
			elog(ERROR, "Internal error");
	}
	return ret;
}

const char *
conv_nodetype(const char *src, pgsp_parser_mode mode)
{
	return converter_core(nodetypes, src, mode);
}

const char *
conv_scandir(const char *src, pgsp_parser_mode mode)
{
	return converter_core(directions, src, mode);
}

const char *
conv_relasionship(const char *src, pgsp_parser_mode mode)
{
	return converter_core(relationships, src, mode);
}

const char *
conv_strategy(const char *src, pgsp_parser_mode mode)
{
	return converter_core(strategies, src, mode);
}

/*
 * Look for these operator characters in order to decide whether to strip
 * whitespaces which are needless from the view of sql syntax in
 * normalize_expr(). This must be synced with op_chars in scan.l.
 */
#define OPCHARS "~!@#^&|`?+-*/%<>="
#define IS_WSCHAR(c) ((c) == ' ' || (c) == '\n' || (c) == '\t')
#define IS_CONST(tok) (tok == FCONST || tok == SCONST || tok == BCONST || \
			tok == XCONST || tok == ICONST || tok == NULL_P || \
		    tok == TRUE_P || tok == FALSE_P || \
			tok == CURRENT_DATE || tok == CURRENT_TIME || \
		    tok == LOCALTIME || tok == LOCALTIMESTAMP)
#define IS_INDENTED_ARRAY(v) ((v) == P_GroupKeys || (v) == P_HashKeys)

/*
 * norm_yylex: core_yylex with replacing some tokens.
 */
static int
norm_yylex(char *str, core_YYSTYPE *yylval, YYLTYPE *yylloc, core_yyscan_t yyscanner)
{
	int tok;

	PG_TRY();
	{
		tok = core_yylex(yylval, yylloc, yyscanner);
	}
	PG_CATCH();
	{
		/*
		 * Error might occur during parsing quoted tokens that chopped
		 * halfway. Just ignore the rest of this query even if there might
		 * be other reasons for parsing to fail.
		 */
		FlushErrorState();
		return -1;
	}
	PG_END_TRY();

	/*
	 * '?' alone is assumed to be an IDENT.  If there's a real
	 * operator '?', this should be confused but there's hardly be.
	 */
	if (tok == Op && str[*yylloc] == '?' &&
		strchr(OPCHARS, str[*yylloc + 1]) == NULL)
		tok = SCONST;

	/*
	 * Replace tokens with '=' if the operator is consists of two or
	 * more opchars only. Assuming that opchars do not compose a token
	 * with non-opchars, check the first char only is sufficient.
	 */
	if (tok == Op && strchr(OPCHARS, str[*yylloc]) != NULL)
		tok = '=';

	return tok;
}

/*
 * normalize_expr - Normalize statements or expressions.
 *
 * Mask constants, strip unnecessary whitespaces and upcase keywords. expr is
 * modified in-place (destructively). If readability is more important than
 * uniqueness, preserve_space puts one space for one existent whitespace for
 * more readability.
 */
/* scanner interface is changed in PG12 */
#if PG_VERSION_NUM < 120000
#define ScanKeywords (*ScanKeywords)
#define ScanKeywordTokens NumScanKeywords
#endif
void
normalize_expr(char *expr, bool preserve_space)
{
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	YYLTYPE		lastloc;
	YYLTYPE start;
	char *wp;
	int			tok, lasttok;

	wp = expr;
	yyscanner = scanner_init(expr,
							 &yyextra,
							 &ScanKeywords,
							 ScanKeywordTokens);

	/*
	 * The warnings about nonstandard escape strings is already emitted in the
	 * core. Just silence them here.
	 */
#if PG_VERSION_NUM >= 90500
	yyextra.escape_string_warning = false;
#endif
	lasttok = 0;
	lastloc = -1;

	for (;;)
	{
		tok = norm_yylex(expr, &yylval, &yylloc, yyscanner);

		start = yylloc;

		if (lastloc >= 0)
		{
			int i, i2;

			/* Skipping preceding whitespaces */
			for(i = lastloc ; i < start && IS_WSCHAR(expr[i]) ; i++);

			/* Searching for trailing whitespace */
			for(i2 = i; i2 < start && !IS_WSCHAR(expr[i2]) ; i2++);

			if (lasttok == IDENT)
			{
				/* Identifiers are copied in case-sensitive manner. */
				memcpy(wp, expr + i, i2 - i);
				wp += i2 - i;
			}
#if PG_VERSION_NUM >= 100000
			/*
			 * Since PG10 pg_stat_statements doesn't store trailing semicolon
			 * in the column "query". Normalization is basically useless in the
			 * version but still usefull to match utility commands so follow
			 * the behavior change.
			 */
			else if (lasttok == ';')
			{
				/* Just do nothing */
			}
#endif
			else
			{
				/* Upcase keywords */
				char *sp;
				for (sp = expr + i ; sp < expr + i2 ; sp++, wp++)
					*wp = (*sp >= 'a' && *sp <= 'z' ?
						   *sp - ('a' - 'A') : *sp);
			}

			/*
			 * Because of destructive writing, wp must not go advance the
			 * reading point.
			 * Although this function's output does not need any validity as a
			 * statement or an expression, spaces are added where it should be
			 * to keep some extent of sanity.  If readability is more important
			 * than uniqueness, preserve_space adds one space for each
			 * existent whitespace.
			 */
			if (tok > 0 &&
				i2 < start &&
				(preserve_space ||
				 (tok >= IDENT && lasttok >= IDENT &&
				  !IS_CONST(tok) && !IS_CONST(lasttok))))
				*wp++ = ' ';

			start = i2;
		}

		/* Exit on parse error. */
		if (tok < 0)
		{
			*wp = 0;
			return;
		}

		/*
		 * Negative signs before numbers are tokenized separately. And
		 * explicit positive signs won't appear in deparsed expressions.
		 */
		if (tok == '-')
			tok = norm_yylex(expr, &yylval, &yylloc, yyscanner);

		/* Exit on parse error. */
		if (tok < 0)
		{
			*wp = 0;
			return;
		}

		if (IS_CONST(tok))
		{
			YYLTYPE end;

			tok = norm_yylex(expr, &yylval, &end, yyscanner);

			/* Exit on parse error. */
			if (tok < 0)
			{
				*wp = 0;
				return;
			}

			/*
			 * Negative values may be surrounded with parens by the
			 * deparser. Mask involving them.
			 */
			if (lasttok == '(' && tok == ')')
			{
				wp -= (start - lastloc);
				start = lastloc;
				end++;
			}

			while (expr[end - 1] == ' ')
				end--;

			*wp++ = '?';
			yylloc = end;
		}

		if (tok == 0)
			break;

		lasttok = tok;
		lastloc = yylloc;
	}
	*wp = 0;
}

const char *
conv_expression(const char *src, pgsp_parser_mode mode)
{
	const char *ret = src;

	if (mode == PGSP_JSON_NORMALIZE)
	{
		char *t = pstrdup(src);
		normalize_expr(t, true);
		ret = (const char *)t;
	}
	return ret;
}

const char *
conv_operation(const char *src, pgsp_parser_mode mode)
{
	return converter_core(operations, src, mode);

}

const char *
conv_jointype(const char *src, pgsp_parser_mode mode)
{
	return converter_core(jointypes, src, mode);
}

const char *
conv_setsetopcommand(const char *src, pgsp_parser_mode mode)
{
	return converter_core(setsetopcommands, src, mode);
}

const char *
conv_sortmethod(const char *src, pgsp_parser_mode mode)
{
	return converter_core(sortmethods, src, mode);
}

const char *
conv_sortspacetype(const char *src, pgsp_parser_mode mode)
{
	return converter_core(sortspacetype, src, mode);
}

const char *
conv_partialmode(const char *src, pgsp_parser_mode mode)
{
	return converter_core(partialmode, src, mode);
}

/**** Parser callbacks ****/

/* JSON */
static void
json_objstart(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (ctx->mode == PGSP_JSON_INFLATE)
	{
		if (!ctx->fname && ctx->dest->len > 0)
		{
			appendStringInfoChar(ctx->dest, '\n');
			appendStringInfoSpaces(ctx->dest, (ctx->level) * INDENT_STEP);
		}
		ctx->fname = NULL;
	}
	appendStringInfoChar(ctx->dest, '{');

	ctx->level++;
	ctx->first = bms_add_member(ctx->first, ctx->level);

	if (ctx->mode == PGSP_JSON_INFLATE)
		appendStringInfoChar(ctx->dest, '\n');
}

static void
json_objend(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	if (ctx->mode == PGSP_JSON_INFLATE)
	{
		if (!bms_is_member(ctx->level, ctx->first))
			appendStringInfoChar(ctx->dest, '\n');
		appendStringInfoSpaces(ctx->dest, (ctx->level - 1) * INDENT_STEP);
	}

	appendStringInfoChar(ctx->dest, '}');

	ctx->level--;
	ctx->last_elem_is_object = true;
	ctx->first = bms_del_member(ctx->first, ctx->level);
	ctx->fname = NULL;
}

static void
json_arrstart(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (IS_INDENTED_ARRAY(ctx->current_list))
		ctx->wlist_level++;

	appendStringInfoChar(ctx->dest, '[');
	ctx->fname = NULL;
	ctx->level++;
	ctx->last_elem_is_object = true;
	ctx->first = bms_add_member(ctx->first, ctx->level);
}

static void
json_arrend(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (IS_INDENTED_ARRAY(ctx->current_list))
		ctx->wlist_level--;

	if (ctx->mode == PGSP_JSON_INFLATE &&
		(IS_INDENTED_ARRAY(ctx->current_list) ?
		 ctx->wlist_level == 0 : ctx->last_elem_is_object))
	{
		appendStringInfoChar(ctx->dest, '\n');
		appendStringInfoSpaces(ctx->dest, (ctx->level - 1) * INDENT_STEP);
	}

	appendStringInfoChar(ctx->dest, ']');
	ctx->level--;
}

static void
json_ofstart(void *state, char *fname, bool isnull)
{
	word_table *p;
	pgspParserContext *ctx = (pgspParserContext *)state;
	char *fn;

	ctx->remove = false;
	p = search_word_table(propfields, fname, ctx->mode);
	if (!p)
	{
		ereport(DEBUG1,
				(errmsg("JSON parser encoutered unknown field name: \"%s\".", fname),
				 errdetail_log("INPUT: \"%s\"", ctx->org_string)));
	}

	ctx->remove = (ctx->mode == PGSP_JSON_NORMALIZE &&
				   (!p || !p->normalize_use));

	if (ctx->remove)
		return;

	if (!bms_is_member(ctx->level, ctx->first))
	{
		appendStringInfoChar(ctx->dest, ',');
		if (ctx->mode == PGSP_JSON_INFLATE)
			appendStringInfoChar(ctx->dest, '\n');
	}
	else
		ctx->first = bms_del_member(ctx->first, ctx->level);

	if (ctx->mode == PGSP_JSON_INFLATE)
		appendStringInfoSpaces(ctx->dest, ctx->level * INDENT_STEP);

	/*
	 * We intentionally let some property names not have a short name. Use long
	 * name for the cases.
	 */
	if (!p || !p->longname)
		fn = fname;
	else if (ctx->mode == PGSP_JSON_INFLATE ||
			 !(p->shortname && p->shortname[0]))
		fn = p->longname;
	else
		fn = p->shortname;

	escape_json(ctx->dest, fn);
	ctx->fname = fn;
	ctx->valconverter = (p ? p->converter : NULL);

	appendStringInfoChar(ctx->dest, ':');

	if (ctx->mode == PGSP_JSON_INFLATE)
		appendStringInfoChar(ctx->dest, ' ');

	if (p && IS_INDENTED_ARRAY(p->tag))
	{
		ctx->current_list = p->tag;
		ctx->list_fname = fname;
		ctx->wlist_level = 0;
	}
}

static void
json_ofend(void *state, char *fname, bool isnull)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (ctx->list_fname && strcmp(fname, ctx->list_fname) == 0)
	{
		ctx->list_fname = NULL;
		ctx->current_list = P_Invalid;
	}
}

static void
json_aestart(void *state, bool isnull)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	if (ctx->remove)
		return;

	if (IS_INDENTED_ARRAY(ctx->current_list) &&
		ctx->wlist_level == 1)
	{
		if (!bms_is_member(ctx->level, ctx->first))
			appendStringInfoChar(ctx->dest, ',');

		if (ctx->mode == PGSP_JSON_INFLATE)
		{
			appendStringInfoChar(ctx->dest, '\n');
			appendStringInfoSpaces(ctx->dest, (ctx->level) * INDENT_STEP);
		}
	}
	else
	{
		if (!bms_is_member(ctx->level, ctx->first))
		{
			appendStringInfoChar(ctx->dest, ',');

			if (ctx->mode == PGSP_JSON_INFLATE &&
				!ctx->last_elem_is_object)
				appendStringInfoChar(ctx->dest, ' ');
		}
	}

	ctx->first = bms_del_member(ctx->first, ctx->level);
}

static void
json_scalar(void *state, char *token, JsonTokenType tokentype)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	const char *val = token;

	if (ctx->remove)
		return;

	if (ctx->valconverter)
		val = ctx->valconverter(token, ctx->mode);

	if (tokentype == JSON_TOKEN_STRING)
		escape_json(ctx->dest, val);
	else
		appendStringInfoString(ctx->dest, val);
	ctx->last_elem_is_object = false;
}


/* YAML */
static void
yaml_objstart(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (ctx->fname)
	{
		if (ctx->dest->len > 0)
			appendStringInfoChar(ctx->dest, '\n');
		appendStringInfoSpaces(ctx->dest, (ctx->level - 1) * INDENT_STEP);
		appendStringInfoString(ctx->dest, "- ");
		appendStringInfoString(ctx->dest, ctx->fname);
		appendStringInfoString(ctx->dest, ":\n");
		appendStringInfoSpaces(ctx->dest, (ctx->level + 1) * INDENT_STEP);
		ctx->fname = NULL;
	}

	ctx->level++;
	ctx->first = bms_add_member(ctx->first, ctx->level);
}

static void
yaml_objend(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	ctx->level--;
	ctx->last_elem_is_object = true;
	ctx->first = bms_del_member(ctx->first, ctx->level);
}

static void
yaml_arrstart(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (ctx->fname)
	{
		appendStringInfoString(ctx->dest, ctx->fname);
		appendStringInfoString(ctx->dest, ":");
	}

	ctx->fname = NULL;
	ctx->level++;
	ctx->first = bms_add_member(ctx->first, ctx->level);
}

static void
yaml_arrend(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	ctx->level--;
}
static void
yaml_ofstart(void *state, char *fname, bool isnull)
{
	word_table *p;
	pgspParserContext *ctx = (pgspParserContext *)state;
	char *s;

	p = search_word_table(propfields, fname, ctx->mode);
	if (!p)
	{
		ereport(DEBUG1,
				(errmsg("Short JSON parser encoutered unknown field name: \"%s\".", fname),
				 errdetail_log("INPUT: \"%s\"", ctx->org_string)));
	}
	s = (p ? p->longname : fname);

	if (!bms_is_member(ctx->level, ctx->first))
	{
		appendStringInfoString(ctx->dest, "\n");
		appendStringInfoSpaces(ctx->dest, ctx->level * INDENT_STEP);
	}
	else
		ctx->first = bms_del_member(ctx->first, ctx->level);

	ctx->valconverter = NULL;
	ctx->fname = s;
	ctx->valconverter = (p ? p->converter : NULL);
}

static void
yaml_aestart(void *state, bool isnull)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	appendStringInfoString(ctx->dest, "\n");
	bms_del_member(ctx->first, ctx->level);
	appendStringInfoSpaces(ctx->dest, ctx->level * INDENT_STEP);
	appendStringInfoString(ctx->dest, "- ");
}

static void
yaml_scalar(void *state, char *token, JsonTokenType tokentype)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (ctx->fname)
	{
		appendStringInfoString(ctx->dest, ctx->fname);
		appendStringInfoString(ctx->dest, ": ");
		ctx->fname = NULL;
	}

	json_scalar(state, token, tokentype);

	ctx->last_elem_is_object = false;
}


/* XML */
static void
xml_objstart(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	ctx->level ++;
	ctx->first = bms_add_member(ctx->first, ctx->level);
}


static void
xml_objend(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	appendStringInfoChar(ctx->dest, '\n');
	appendStringInfoSpaces(ctx->dest, ctx->level * INDENT_STEP);

	ctx->level--;
	ctx->first = bms_del_member(ctx->first, ctx->level);

	ctx->last_elem_is_object = true;
}

static void
xml_arrend(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	appendStringInfoChar(ctx->dest, '\n');
	appendStringInfoSpaces(ctx->dest, (ctx->level + 1) * INDENT_STEP);
}

static void
adjust_wbuf(pgspParserContext *ctx, int len)
{
	int buflen;

	for (buflen = ctx->wbuflen ; len > buflen ; buflen *= 2);
	if (buflen > ctx->wbuflen)
	{
		ctx->wbuf = (char *)palloc(buflen);
		ctx->wbuflen = buflen;
	}
}

static char *
hyphenate_words(pgspParserContext *ctx, char *src)
{
	char *p;

	adjust_wbuf(ctx, strlen(src) + 1);
	strcpy(ctx->wbuf, src);

	for (p = ctx->wbuf ; *p ; p++)
		if (*p == ' ') *p = '-';

	return ctx->wbuf;
}

static void
xml_ofstart(void *state, char *fname, bool isnull)
{
	word_table *p;
	pgspParserContext *ctx = (pgspParserContext *)state;
	char *s;

	p = search_word_table(propfields, fname, ctx->mode);
	if (!p)
	{
		ereport(DEBUG1,
				(errmsg("Short JSON parser encoutered unknown field name: \"%s\".", fname),
				 errdetail_log("INPUT: \"%s\"", ctx->org_string)));
	}
	s = (p ? p->longname : fname);

	/*
	 * save current process context
	 * There's no problem if P_Plan appears recursively.
	 */
	if (p && (p->tag == P_Plan || p->tag == P_Triggers))
		ctx->section = p->tag;

	appendStringInfoChar(ctx->dest, '\n');
	appendStringInfoSpaces(ctx->dest, (ctx->level + 1) * INDENT_STEP);

	ctx->valconverter = NULL;

	appendStringInfoChar(ctx->dest, '<');
	appendStringInfoString(ctx->dest, escape_xml(hyphenate_words(ctx, s)));
	appendStringInfoChar(ctx->dest, '>');
	ctx->valconverter = (p ? p->converter : NULL);

	/*
	 * If the object field name is Plan or Triggers, the value should be an
	 * array and the items are tagged by other than "Item". "Item"s appear
	 * only in Output field.
	 */
	if (p && (p->tag == P_Plans || p->tag == P_Triggers))
		ctx->not_item = bms_add_member(ctx->not_item, ctx->level + 1);
	else
		ctx->not_item = bms_del_member(ctx->not_item, ctx->level + 1);
}

static void
xml_ofend(void *state, char *fname, bool isnull)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	word_table *p;
	char *s;

	p =	search_word_table(propfields, fname, ctx->mode);
	s = (p ? p->longname : fname);

	appendStringInfoString(ctx->dest, "</");
	appendStringInfoString(ctx->dest, escape_xml(hyphenate_words(ctx, s)));
	appendStringInfoChar(ctx->dest, '>');
}

static void
xml_aestart(void *state, bool isnull)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	char *tag;

	/*
	 * The "Trigger" in "Triggers", "Plan" in "Plans" and "Item" nodes are
	 * implicitly represented in JSON format.  Restore them for XML format.
	 */

	ctx->level++;
	if (bms_is_member(ctx->level, ctx->not_item))
	{
		if (ctx->section == P_Plan)
			tag = "<Plan>";
		else
			tag = "<Trigger>";
	}
	else
		tag = "<Item>";

	appendStringInfoChar(ctx->dest, '\n');
	appendStringInfoSpaces(ctx->dest, (ctx->level + 1) * INDENT_STEP);
	appendStringInfoString(ctx->dest, tag);
}

static void
xml_aeend(void *state, bool isnull)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	char *tag;

	/*
	 * The "Plan" in "Plans" or "Item" nodes are implicitly represented in
	 * JSON format.  Restore it for XML format.
	 */

	if (bms_is_member(ctx->level, ctx->not_item))
	{
		if (ctx->section == P_Plan)
			tag = "</Plan>";
		else
			tag = "</Trigger>";
	}
	else
		tag = "</Item>";
	appendStringInfoString(ctx->dest, tag);
	ctx->level--;
}

static void
xml_scalar(void *state, char *token, JsonTokenType tokentype)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	const char *s = token;

	if (ctx->valconverter)
		s = ctx->valconverter(token, PGSP_JSON_XMLIZE);

	if (tokentype == JSON_TOKEN_STRING)
		s = escape_xml(s);

	appendStringInfoString(ctx->dest, s);
	ctx->last_elem_is_object = false;
}

/********************************/
void
init_parser_context(pgspParserContext *ctx, int mode,
					   char *orgstr, char *buf, int buflen){
	memset(ctx, 0, sizeof(*ctx));
	ctx->dest = makeStringInfo();
	ctx->mode = mode;
	ctx->org_string = orgstr;
	ctx->wbuf = buf;
	ctx->wbuflen = buflen;
}

/*
 * run_pg_parse_json:
 *
 * Wrap pg_parse_json in order to restore InterruptHoldoffCount when parse
 * error occured.
 *
 * Returns true when parse completed. False for unexpected end of string.
 */
bool
run_pg_parse_json(JsonLexContext *lex, JsonSemAction *sem)
{
#if PG_VERSION_NUM >= 130000
	return pg_parse_json(lex, sem) == JSON_SUCCESS;
#else
	MemoryContext ccxt = CurrentMemoryContext;
	uint32 saved_IntrHoldoffCount;

	/*
	 * "ereport(ERROR.." occurs on error in pg_parse_json resets
	 * InterruptHoldoffCount to zero, so we must save the value before calling
	 * json parser to restore it on parse error. See errfinish().
	 */
	saved_IntrHoldoffCount = InterruptHoldoffCount;

	PG_TRY();
	{
		pg_parse_json(lex, sem);
	}
	PG_CATCH();
	{
		ErrorData *errdata;
		MemoryContext ecxt;

		InterruptHoldoffCount = saved_IntrHoldoffCount;

		ecxt = MemoryContextSwitchTo(ccxt);
		errdata = CopyErrorData();

		if (errdata->sqlerrcode == ERRCODE_INVALID_TEXT_REPRESENTATION)
		{
			FlushErrorState();
			return false;
		}
		else
		{
			MemoryContextSwitchTo(ecxt);
			PG_RE_THROW();
		}
	}
	PG_END_TRY();

	return true;
#endif
}

void
init_json_lex_context(JsonLexContext *lex, char *json)
{
	lex->input = lex->token_terminator = lex->line_start = json;
	lex->line_number = 1;
	lex->input_length = strlen(json);
	lex->strval = makeStringInfo();
}

static void
init_json_semaction(JsonSemAction *sem, pgspParserContext *ctx)
{
	sem->semstate = (void*)ctx;
	sem->object_start       = json_objstart;
	sem->object_end         = json_objend;
	sem->array_start        = json_arrstart;
	sem->array_end          = json_arrend;
	sem->object_field_start = json_ofstart;
	sem->object_field_end   = json_ofend;
	sem->array_element_start= json_aestart;
	sem->array_element_end  = NULL;
	sem->scalar             = json_scalar;
}

char *
pgsp_json_shorten(char *json)
{
	JsonLexContext lex;
	JsonSemAction sem;
	pgspParserContext    ctx;

	init_json_lex_context(&lex, json);
	init_parser_context(&ctx, PGSP_JSON_SHORTEN, json, NULL, 0);
	init_json_semaction(&sem, &ctx);

	run_pg_parse_json(&lex, &sem);

	return ctx.dest->data;
}

char *
pgsp_json_normalize(char *json)
{
	JsonLexContext lex;
	JsonSemAction sem;
	pgspParserContext    ctx;

	init_json_lex_context(&lex, json);
	init_parser_context(&ctx,PGSP_JSON_NORMALIZE, json, NULL, 0);
	init_json_semaction(&sem, &ctx);

	run_pg_parse_json(&lex, &sem);

	return ctx.dest->data;
}

char *
pgsp_json_inflate(char *json)
{
	JsonLexContext lex;
	JsonSemAction sem;
	pgspParserContext    ctx;

	init_json_lex_context(&lex, json);
	init_parser_context(&ctx, PGSP_JSON_INFLATE, json, NULL, 0);
	init_json_semaction(&sem, &ctx);

	if (!run_pg_parse_json(&lex, &sem))
	{
		if (ctx.dest->len > 0 &&
			ctx.dest->data[ctx.dest->len - 1] != '\n')
			appendStringInfoChar(ctx.dest, '\n');

		if (ctx.dest->len == 0)
			appendStringInfoString(ctx.dest, "<Input was not JSON>");
		else
			appendStringInfoString(ctx.dest, "<truncated>");
	}

	return ctx.dest->data;
}

char *
pgsp_json_yamlize(char *json)
{
	pgspParserContext    ctx;
	JsonSemAction sem;
	JsonLexContext lex;

	init_json_lex_context(&lex, json);
	init_parser_context(&ctx, PGSP_JSON_YAMLIZE, json, NULL, 0);

	sem.semstate = (void*)&ctx;
	sem.object_start       = yaml_objstart;
	sem.object_end         = yaml_objend;
	sem.array_start        = yaml_arrstart;
	sem.array_end          = yaml_arrend;
	sem.object_field_start = yaml_ofstart;
	sem.object_field_end   = NULL;
	sem.array_element_start= yaml_aestart;
	sem.array_element_end  = NULL;
	sem.scalar             = yaml_scalar;

	if (!run_pg_parse_json(&lex, &sem))
	{
		if (ctx.dest->len > 0 &&
			ctx.dest->data[ctx.dest->len - 1] != '\n')
			appendStringInfoChar(ctx.dest, '\n');

		if (ctx.dest->len == 0)
			appendStringInfoString(ctx.dest, "<Input was not JSON>");
		else
			appendStringInfoString(ctx.dest, "<truncated>");
	}

	return ctx.dest->data;
}

char *
pgsp_json_xmlize(char *json)
{
	pgspParserContext      ctx;
	JsonSemAction sem;
	JsonLexContext lex;
	int start_len;
	char buf[32];

	init_json_lex_context(&lex, json);
	init_parser_context(&ctx, PGSP_JSON_XMLIZE, json, buf, sizeof(buf));

	sem.semstate = (void*)&ctx;
	sem.object_start       = xml_objstart;
	sem.object_end         = xml_objend;
	sem.array_start        = NULL;
	sem.array_end          = xml_arrend;
	sem.object_field_start = xml_ofstart;
	sem.object_field_end   = xml_ofend;
	sem.array_element_start= xml_aestart;
	sem.array_element_end  = xml_aeend;
	sem.scalar             = xml_scalar;

	appendStringInfo(ctx.dest,
					 "<explain xmlns=\"http://www.postgresql.org/2009/explain\">\n  <Query>");
	start_len = ctx.dest->len;

	if (!run_pg_parse_json(&lex, &sem))
	{
		if (ctx.dest->len > start_len &&
			ctx.dest->data[ctx.dest->len - 1] != '\n')
			appendStringInfoChar(ctx.dest, '\n');

		if (ctx.dest->len == start_len)
		{
			resetStringInfo(ctx.dest);
			appendStringInfoString(ctx.dest, "<Input was not JSON>");
		}
		else
			appendStringInfoString(ctx.dest, "<truncated>");
	}
	else
		appendStringInfo(ctx.dest, "</Query>\n</explain>\n");

	return ctx.dest->data;
}
