/*-------------------------------------------------------------------------
 *
 * pgsp_json_int.h: Definitions for internal use for pgsp_json.c
 *
 * Copyright (c) 2012-2021, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plans/pgsp_json_int.h
 *
 *-------------------------------------------------------------------------
 */

typedef enum
{
	PGSP_JSON_SHORTEN,
	PGSP_JSON_INFLATE,
	PGSP_JSON_TEXTIZE,
	PGSP_JSON_YAMLIZE,
	PGSP_JSON_XMLIZE,
	PGSP_JSON_NORMALIZE
} pgsp_parser_mode;

typedef enum 
{
	S_Invalid,
	S_Plain,
	S_Sorted,
	S_Hashed,
	S_Mixed
} pgsp_strategies;

typedef const char *(converter_t)(const char *src, pgsp_parser_mode mode);
typedef void (setter_t)(node_vals *vals, const char *val);

typedef enum
{
	P_Invalid,
	P_Plan,
	P_Plans,
	P_NodeType,
	P_RelationShip,
	P_ScanDir,
	P_IndexName,
	P_RelationName,
	P_FunctioName,
	P_CTEName,
	P_Schema,
	P_Alias,
	P_Output,
	P_MergeCond,
	P_Strategy,
	P_JoinType,
	P_Command,
	P_SortMethod,
	P_SortKey,
	P_GroupKey,
	P_GroupKeys,
	P_GroupSets,
	P_HashKeys,
	P_HashKey,
	P_Filter,
	P_JoinFilter,
	P_HashCond,
	P_IndexCond,
	P_TidCond,
	P_RecheckCond,
	P_Operation,
	P_SubplanName,
	P_Triggers,
	P_Trigger,
	P_TriggerName,
	P_TrgRelation,
	P_ConstraintName,
	P_Parallel,
	P_PartialMode,
	P_WorkersPlanned,

	P_FunctionCall,
	P_StartupCost,
	P_TotalCost,
	P_PlanRows,
	P_PlanWidth,
	P_ActualStartupTime,
	P_ActualTotalTime,
	P_ActualRows,
	P_ActualLoops,
	P_HeapFetches,
	P_SharedHitBlks,
	P_SharedReadBlks,
	P_SharedDirtiedBlks,
	P_SharedWrittenBlks,
	P_LocalHitBlks,
	P_LocalReadBlks,
	P_LocalDirtiedBlks,
	P_LocalWrittenBlks,
	P_TempReadBlks,
	P_TempWrittenBlks,
	P_IOReadTime,
	P_IOWwriteTime,
	P_SortSpaceUsed,
	P_SortSpaceType,
	P_PeakMemoryUsage,
	P_OrgHashBatches,
	P_OrgHashBuckets,
	P_HashBatches,
	P_HashBuckets,
	P_RowsFilterRmvd,
	P_RowsIdxRchkRmvd,
	P_TrgTime,
	P_TrgCalls,
	P_PlanTime,
	P_ExecTime,
	P_ExactHeapBlks,
	P_LossyHeapBlks,
	P_RowsJoinFltRemvd,
	P_TargetTables,
	P_ConfRes,
	P_ConfArbitIdx,
	P_TuplesInserted,
	P_ConfTuples,
	P_SamplingMethod,
	P_SamplingParams,
	P_RepeatableSeed,
	P_Workers,
	P_WorkersLaunched,
	P_WorkerNumber,
	P_InnerUnique,
	P_TableFuncName,
	P_PresortedKey,
	P_FullsortGroups,
	P_SortMethodsUsed,
	P_SortSpaceMemory,
	P_GroupCount,
	P_AvgSortSpcUsed,
	P_PeakSortSpcUsed,
	P_PreSortedGroups,
	P_AsyncCapable
} pgsp_prop_tags;

typedef struct
{
	int	  tag;				/* Tag to identify words */
	char *shortname;		/* Property name for short-style JSON */
	char *longname;			/* Property name for long(normal)-style JSON */
	char *textname;			/* Property name for Text representation */
	bool  normalize_use;	/* True means this word to be used for
							   normalization, which makes difference of
							   plan-id */
	converter_t *converter;	/* Converter function for the property name */
	setter_t  *setter;		/* Converter function for the property value */
} word_table;

typedef struct
{
	StringInfo	dest;			/* Storage for parse result */
	pgsp_parser_mode mode;		/* Tells what to do to the parser */
	node_vals *nodevals;		/* Node value holder */
	char	   *org_string;		/* What to parse */

	/* Working variables used internally in parser */
	int			level;			/* Next (indent or object) level */
	Bitmapset  *plan_levels;	/* Level list for Plan objects */
	Bitmapset  *first;			/* Bitmap set holds whether the first element
								 * has been processed for each level */
	Bitmapset  *not_item;		/* Bitmap set holds whether the node name at
								   the level was literally "Item" or not. */
	bool		remove;			/* True if the current node is not shown in
								 * the result */
	bool		last_elem_is_object; /* True if the last processed element
								 * was an object */
	pgsp_prop_tags	section;	/* explain section under processing */
	pgsp_prop_tags	current_list; /* current list tag that needs special treat*/
	StringInfo work_str;		/* StringInfor for very-short term usage */
	char	   *list_fname;		/* the field name of the current_list */
	char	   *fname;			/* Field name*/
	char	   *wbuf;			/* Working buffer */
	int			wbuflen;		/* Length of the working buffer */
	int			wlist_level;	/* Nest level of list for Grouping Sets */
	grouping_set *tmp_gset;	/* Working area for grouping sets */

	converter_t *valconverter;	/* field name converter for the current
								 * element */
	setter_t    *setter;		/* value converter for the current element */
} pgspParserContext;


extern word_table nodetypes[];
extern word_table strategies[];
extern word_table propfields[];

extern void init_word_index(void);
extern word_table *search_word_table(word_table *tbl,
										  const char *word, int mode);
extern const char *conv_nodetype(const char *src, pgsp_parser_mode mode);
extern const char *conv_operation(const char *src, pgsp_parser_mode mode);
extern const char *conv_scandir(const char *src, pgsp_parser_mode mode);
extern const char *conv_expression(const char *src, pgsp_parser_mode mode);
extern const char *conv_relasionship(const char *src, pgsp_parser_mode mode);
extern const char *conv_jointype(const char *src, pgsp_parser_mode mode);
extern const char *conv_strategy(const char *src, pgsp_parser_mode mode);
extern const char *conv_setsetopcommand(const char *src, pgsp_parser_mode mode);
extern const char *conv_sortmethod(const char *src, pgsp_parser_mode mode);
extern const char *conv_sortspacetype(const char *src, pgsp_parser_mode mode);
extern const char *conv_partialmode(const char *src, pgsp_parser_mode mode);

extern bool run_pg_parse_json(JsonLexContext *lex, JsonSemAction *sem);
extern void init_parser_context(pgspParserContext *ctx, int mode,
								   char *orgstr, char *buf,int buflen);
extern void init_json_lex_context(JsonLexContext *lex, char *json);

