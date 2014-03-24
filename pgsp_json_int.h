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
	S_Hashed
} pgsp_strategies;

typedef const char *(converter_t)(const char *src, pgsp_parser_mode mode);
typedef void (setter_t)(node_vals *vals, const char *val);

typedef struct
{
	int   tag;
	char *shortname;
	char *longname;
	char *textname;
	bool  normalize_use;
	converter_t *converter;
	setter_t  *setter;
} word_table;

typedef struct
{
	StringInfo	dest;
	pgsp_parser_mode mode;
	node_vals *nodevals;
	char	   *org_string;

	/* Working variables used internally in parser */
	int			level;
	Bitmapset  *first;
	Bitmapset  *not_item;
	bool		remove;
	bool		last_elem_is_object;
	bool		processing;
	char	   *fname;
	char	   *wbuf;
	int			wbuflen;
	converter_t *valconverter;
	setter_t    *setter;
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

extern bool run_pg_parse_json(JsonLexContext *lex, JsonSemAction *sem);
extern void init_parser_context(pgspParserContext *ctx, int mode,
								   char *orgstr, char *buf,int buflen);
extern void init_json_lex_context(JsonLexContext *lex, char *json);

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
	P_HashBatches,
	P_HashBuckets,
	P_RowsFilterRmvd,
	P_RowsIdxRchkRmvd,
	P_TrgTime,
	P_TrgCalls
} pgsp_prop_tags;
