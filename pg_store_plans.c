/*-------------------------------------------------------------------------
 *
 * pg_store_plans
 *
 * Copyright (c) 2008-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_store_plan/pg_store_plan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <dlfcn.h>

#include "commands/explain.h"
#include "access/hash.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "pgsp_json.h"
#include "pgsp_explain.h"

PG_MODULE_MAGIC;

/* Location of stats file */
#define PGSP_DUMP_FILE	"global/pg_store_plans.stat"

/* This constant defines the magic number in the stats file header */
static const uint32 PGSP_FILE_HEADER = 0x20130828;
static const uint32 pg_store_plan_size = 5000;

/* XXX: Should USAGE_EXEC reflect execution time and/or buffer usage? */
#define USAGE_EXEC(duration)	(1.0)
#define USAGE_INIT				(1.0)	/* including initial planning */
#define ASSUMED_MEDIAN_INIT		(10.0)	/* initial assumed median usage */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every entry_dealloc */
#define STICKY_DECREASE_FACTOR	(0.50)	/* factor for sticky entries */
#define USAGE_DEALLOC_PERCENT	5		/* free this % of entries at once */

/*
 * Hashtable key that defines the identity of a hashtable entry.  We separate
 * queries by user and by database even if they are otherwise identical.
 *
 * Presently, the query encoding is fully determined by the source database
 * and so we don't really need it to be in the key.  But that might not always
 * be true. Anyway it's notationally convenient to pass it as part of the key.
 */
typedef struct EntryKey
{
	Oid			userid;			/* user OID */
	Oid			dbid;			/* database OID */
	int			encoding;		/* query encoding */
	uint32		queryid;		/* query identifier */
	uint32		planid;			/* plan identifier */
} EntryKey;

/*
 * The actual stats counters kept within StatEntry.
 */
typedef struct Counters
{
	int64		calls;				/* # of times executed */
	double		total_time;			/* total execution time, in msec */
	int64		rows;				/* total # of retrieved or affected rows */
	int64		shared_blks_hit;	/* # of shared buffer hits */
	int64		shared_blks_read;	/* # of shared disk blocks read */
	int64		shared_blks_dirtied;/* # of shared disk blocks dirtied */
	int64		shared_blks_written;/* # of shared disk blocks written */
	int64		local_blks_hit; 	/* # of local buffer hits */
	int64		local_blks_read;	/* # of local disk blocks read */
	int64		local_blks_dirtied;	/* # of local disk blocks dirtied */
	int64		local_blks_written;	/* # of local disk blocks written */
	int64		temp_blks_read; 	/* # of temp blocks read */
	int64		temp_blks_written;	/* # of temp blocks written */
	double		blk_read_time;		/* time spent reading, in msec */
	double		blk_write_time; 	/* time spent writing, in msec */
	TimestampTz	first_call;			/* timestamp of first call  */
	TimestampTz	last_call;			/* timestamp of last call  */
	double		usage;				/* usage factor */
} Counters;

/*
 * Statistics per plan
 *
 * NB: see the file read/write code before changing field order here.
 */
typedef struct StatEntry
{
	EntryKey	key;			/* hash key of entry - MUST BE FIRST */
	uint32		queryid;			/* query identifier from stat_statements*/
	Counters	counters;		/* the statistics for this query */
	int			plan_len;		/* # of valid bytes in query string */
	slock_t		mutex;			/* protects the counters only */
	char		plan[1];		/* VARIABLE LENGTH ARRAY - MUST BE LAST */
	/*
	 * Note: the allocated length of query[] is actually
	 * shared_state->query_size
	 */
} StatEntry;

/*
 * Global shared state
 */
typedef struct SharedState
{
	LWLockId	lock;			/* protects hashtable search/modification */
	int			plan_size;		/* max query length in bytes */
	double		cur_median_usage;	/* current median usage in hashtable */
} SharedState;

/*---- Local variables ----*/

/* Current nesting depth of ExecutorRun+ProcessUtility calls */
static int	nested_level = 0;

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Links to shared memory state */
static SharedState *shared_state = NULL;
static HTAB *hash_table = NULL;

/*---- GUC variables ----*/

typedef enum
{
	TRACK_LEVEL_NONE,			/* track no statements */
	TRACK_LEVEL_TOP,				/* only top level statements */
	TRACK_LEVEL_ALL,				/* all statements, including nested ones */
	TRACK_LEVEL_FORCE			/* all statements, including nested ones */
}	PGSPTrackLevel;

static const struct config_enum_entry track_options[] =
{
	{"none", TRACK_LEVEL_NONE, false},
	{"top", TRACK_LEVEL_TOP, false},
	{"all", TRACK_LEVEL_ALL, false},
	{NULL, 0, false}
};

typedef enum
{
	PLAN_FORMAT_RAW,		/* No conversion. Shorten JSON */
	PLAN_FORMAT_TEXT,		/* Traditional text representation */
	PLAN_FORMAT_JSON,		/* JSON representation */
	PLAN_FORMAT_YAML,		/* YAML */
	PLAN_FORMAT_XML,		/* XML  */
}	PGSPPlanFormats;

static const struct config_enum_entry plan_formats[] =
{
	{"raw" , PLAN_FORMAT_RAW , false},
	{"text", PLAN_FORMAT_TEXT, false},
	{"json", PLAN_FORMAT_JSON, false},
	{"yaml", PLAN_FORMAT_YAML, false},
	{"xml" , PLAN_FORMAT_XML , false},
	{NULL, 0, false}
};

static int	store_size;			/* max # statements to track */
static int	track_level;		/* tracking level */
static int	min_duration;		/* min duration to record */
static bool dump_on_shutdown;	/* whether to save stats across shutdown */
static bool log_analyze;		/* Similar to EXPLAIN (ANALYZE *) */
static bool log_verbose;		/* Similar to EXPLAIN (VERBOSE *) */
static bool log_buffers;		/* Similar to EXPLAIN (BUFFERS *) */
static bool log_timing;			/* Similar to EXPLAIN (TIMING *) */
static bool log_triggers;		/* whether to log trigger statistics  */
static int  plan_format;	/* Plan representation style in
								 * pg_store_plans.plan  */

#define pgsp_enabled() \
	(track_level == TRACK_LEVEL_ALL || \
	(track_level == TRACK_LEVEL_TOP && nested_level == 0))

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

Datum		pg_store_plans_reset(PG_FUNCTION_ARGS);
Datum		pg_store_plans_hash_query(PG_FUNCTION_ARGS);
Datum		pg_store_plans(PG_FUNCTION_ARGS);
Datum		pg_store_plans_shorten(PG_FUNCTION_ARGS);
Datum		pg_store_plans_normalize(PG_FUNCTION_ARGS);
Datum		pg_store_plans_jsonplan(PG_FUNCTION_ARGS);
Datum		pg_store_plans_yamlplan(PG_FUNCTION_ARGS);
Datum		pg_store_plans_xmlplan(PG_FUNCTION_ARGS);
Datum		pg_store_plans_textplan(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_store_plans_reset);
PG_FUNCTION_INFO_V1(pg_store_plans_hash_query);
PG_FUNCTION_INFO_V1(pg_store_plans);
PG_FUNCTION_INFO_V1(pg_store_plans_shorten);
PG_FUNCTION_INFO_V1(pg_store_plans_normalize);
PG_FUNCTION_INFO_V1(pg_store_plans_jsonplan);
PG_FUNCTION_INFO_V1(pg_store_plans_textplan);
PG_FUNCTION_INFO_V1(pg_store_plans_yamlplan);
PG_FUNCTION_INFO_V1(pg_store_plans_xmlplan);

static void pgsp_shmem_startup(void);
static void pgsp_shmem_shutdown(int code, Datum arg);
static void pgsp_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgsp_ExecutorRun(QueryDesc *queryDesc,
				 ScanDirection direction,
				 long count);
static void pgsp_ExecutorFinish(QueryDesc *queryDesc);
static void pgsp_ExecutorEnd(QueryDesc *queryDesc);
static void pgsp_ProcessUtility(Node *parsetree, const char *queryString,
					ProcessUtilityContext context, ParamListInfo params,
					DestReceiver *dest, char *completionTag);
static uint32 hash_table_fn(const void *key, Size keysize);
static int	match_fn(const void *key1, const void *key2, Size keysize);
static uint32 hash_query(const char* query);
static void store_entry(char *plan, uint32 queryId, uint32 queryId2,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage);
static Size shared_mem_size(void);
static StatEntry *entry_alloc(EntryKey *key, const char *query,
			int plan_len, bool sticky);
static void entry_dealloc(void);
static void entry_reset(void);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_statements functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable("pg_store_plans.max",
	  "Sets the maximum number of plans tracked by pg_store_plans.",
							NULL,
							&store_size,
							1000,
							100,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("pg_store_plans.track",
			   "Selects which plans are tracked by pg_store_plans.",
							 NULL,
							 &track_level,
							 TRACK_LEVEL_TOP,
							 track_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("pg_store_plans.plan_format",
			   "Selects which format to be appied for plan representation in pg_store_plans.",
							 NULL,
							 &plan_format,
							 PLAN_FORMAT_TEXT,
							 plan_formats,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("pg_store_plans.min_duration",
					"Minimum duration to record plan in milliseconds.",
							NULL,
							&min_duration,
							0,
							0,
							INT_MAX,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("pg_store_plans.save",
			   "Save pg_store_plans statistics across server shutdowns.",
							 NULL,
							 &dump_on_shutdown,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_store_plans.log_analyze",
							 "Use EXPLAIN ANALYZE for plan logging.",
							 NULL,
							 &log_analyze,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_store_plans.log_buffers",
							 "Log buffer usage.",
							 NULL,
							 &log_buffers,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_store_plans.log_timing",
							 "Log timings.",
							 NULL,
							 &log_timing,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_store_plans.log_triggers",
							 "Log trigger trace.",
							 NULL,
							 &log_triggers,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_store_plans.log_verbose",
			   "Set VERBOSE for EXPLAIN on logging.",
							 NULL,
							 &log_verbose,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	EmitWarningsOnPlaceholders("pg_store_plans");

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in pgsp_shmem_startup().
	 */
	RequestAddinShmemSpace(shared_mem_size());
	RequestAddinLWLocks(1);

	/*
	 * Install hooks.
	 */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgsp_shmem_startup;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgsp_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pgsp_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pgsp_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgsp_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pgsp_ProcessUtility;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	shmem_startup_hook = prev_shmem_startup_hook;
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
	ExecutorEnd_hook = prev_ExecutorEnd;
	ProcessUtility_hook = prev_ProcessUtility;
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing statistics from file.
 */
static void
pgsp_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;
	FILE	   *file;
	uint32		header;
	int32		num;
	int32		i;
	int			plan_size;
	int			buffer_size;
	char	   *buffer = NULL;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	shared_state = NULL;
	hash_table = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	shared_state = ShmemInitStruct("pg_store_plans",
						   sizeof(SharedState),
						   &found);

	if (!found)
	{
		/* First time through ... */
		shared_state->lock = LWLockAssign();
		shared_state->plan_size = pg_store_plan_size;
		shared_state->cur_median_usage = ASSUMED_MEDIAN_INIT;
	}

	/* Be sure everyone agrees on the hash table entry size */
	plan_size = shared_state->plan_size;

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(EntryKey);
	info.entrysize = offsetof(StatEntry, plan) + plan_size;
	info.hash = hash_table_fn;
	info.match = match_fn;
	hash_table = ShmemInitHash("pg_store_plans hash",
							  store_size, store_size,
							  &info,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(pgsp_shmem_shutdown, (Datum) 0);

	/*
	 * Attempt to load old statistics from the dump file, if this is the first
	 * time through and we weren't told not to.
	 */
	if (found || !dump_on_shutdown)
		return;

	/*
	 * Note: we don't bother with locks here, because there should be no other
	 * processes running when this code is reached.
	 */
	file = AllocateFile(PGSP_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno == ENOENT)
			return;				/* ignore not-found error */
		goto error;
	}

	buffer_size = plan_size;
	buffer = (char *) palloc(buffer_size);

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		header != PGSP_FILE_HEADER ||
		fread(&num, sizeof(int32), 1, file) != 1)
		goto error;

	for (i = 0; i < num; i++)
	{
		StatEntry	temp;
		StatEntry  *entry;

		if (fread(&temp, offsetof(StatEntry, mutex), 1, file) != 1)
			goto error;

		/* Encoding is the only field we can easily sanity-check */
		if (!PG_VALID_BE_ENCODING(temp.key.encoding))
			goto error;

		/* Previous incarnation might have had a larger plan_size */
		if (temp.plan_len >= buffer_size)
		{
			buffer = (char *) repalloc(buffer, temp.plan_len + 1);
			buffer_size = temp.plan_len + 1;
		}

		if (fread(buffer, 1, temp.plan_len, file) != temp.plan_len)
			goto error;
		buffer[temp.plan_len] = '\0';

		/* Skip loading "sticky" entries */
		if (temp.counters.calls == 0)
			continue;

		/* Clip to available length if needed */
		if (temp.plan_len >= plan_size)
			temp.plan_len = pg_encoding_mbcliplen(temp.key.encoding,
												   buffer,
												   temp.plan_len,
												   plan_size - 1);

		/* make the hashtable entry (discards old entries if too many) */
		entry = entry_alloc(&temp.key, buffer, temp.plan_len, false);

		/* copy in the actual stats */
		entry->counters = temp.counters;
	}

	pfree(buffer);
	FreeFile(file);

	/*
	 * Remove the file so it's not included in backups/replication slaves,
	 * etc. A new file will be written on next shutdown.
	 */
	unlink(PGSP_DUMP_FILE);

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read pg_stat_statement file \"%s\": %m",
					PGSP_DUMP_FILE)));
	if (buffer)
		pfree(buffer);
	if (file)
		FreeFile(file);
	/* If possible, throw away the bogus file; ignore any error */
	unlink(PGSP_DUMP_FILE);
}

/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
pgsp_shmem_shutdown(int code, Datum arg)
{
	FILE	   *file;
	HASH_SEQ_STATUS hash_seq;
	int32		num_entries;
	StatEntry  *entry;

	/* Don't try to dump during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!shared_state || !hash_table)
		return;

	/* Don't dump if told not to. */
	if (!dump_on_shutdown)
		return;

	file = AllocateFile(PGSP_DUMP_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&PGSP_FILE_HEADER, sizeof(uint32), 1, file) != 1)
		goto error;
	num_entries = hash_get_num_entries(hash_table);
	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;

	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			len = entry->plan_len;

		if (fwrite(entry, offsetof(StatEntry, mutex), 1, file) != 1 ||
			fwrite(entry->plan, 1, len, file) != len)
			goto error;
	}

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	/*
	 * Rename file into place, so we atomically replace the old one.
	 */
	if (rename(PGSP_DUMP_FILE ".tmp", PGSP_DUMP_FILE) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename pg_store_plans file \"%s\": %m",
						PGSP_DUMP_FILE ".tmp")));

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write pg_store_plans file \"%s\": %m",
					PGSP_DUMP_FILE ".tmp")));
	if (file)
		FreeFile(file);
	unlink(PGSP_DUMP_FILE ".tmp");
}


/*
 * ExecutorStart hook: start up tracking if needed
 */
static void
pgsp_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (log_analyze &&
		(eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
	{
		queryDesc->instrument_options |=
			(log_timing ? INSTRUMENT_TIMER : 0)|
			(log_timing ? 0: INSTRUMENT_ROWS)|
			(log_buffers ? INSTRUMENT_BUFFERS : 0);
	}
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * Set up to track total elapsed time in ExecutorRun. Allocate in per-query
	 * context so as to be free at ExecutorEnd.
	 */
	if (queryDesc->totaltime == NULL && pgsp_enabled())
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
		MemoryContextSwitchTo(oldcxt);
	}
	
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgsp_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, long count)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
		nested_level--;
	}
	PG_CATCH();
	{
		nested_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgsp_ExecutorFinish(QueryDesc *queryDesc)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		nested_level--;
	}
	PG_CATCH();
	{
		nested_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: store results if needed
 */
static void
pgsp_ExecutorEnd(QueryDesc *queryDesc)
{
	if (queryDesc->totaltime)
	{
		InstrEndLoop(queryDesc->totaltime);

		if (pgsp_enabled() &&
			queryDesc->totaltime->total >= 
			(double)min_duration / 1000.0)
		{
			ExplainState es;

			ExplainInitState(&es);
			es.analyze = queryDesc->instrument_options;
			es.costs = true;
			es.verbose = log_verbose;
			es.buffers = (es.analyze && log_buffers);
			es.timing = (es.analyze && log_timing);
			es.format = EXPLAIN_FORMAT_JSON;
	
			ExplainBeginOutput(&es);
			ExplainPrintPlan(&es, queryDesc);
			if (log_triggers)
				pgspExplainTriggers(&es, queryDesc);
			ExplainEndOutput(&es);

			/* Remove last line break */
			if (es.str->len > 0 && es.str->data[es.str->len - 1] == '\n')
				es.str->data[--es.str->len] = '\0';

			/* JSON outmost braces. */
			es.str->data[0] = '{';
			es.str->data[es.str->len - 1] = '}';

			/*
			 * Make sure stats accumulation is done.  (Note: it's okay if several
			 * levels of hook all do this.)
			 */

			store_entry(es.str->data,
						hash_query(queryDesc->sourceText),
						queryDesc->plannedstmt->queryId,
						queryDesc->totaltime->total * 1000.0,	/* convert to msec */
						queryDesc->estate->es_processed,
						&queryDesc->totaltime->bufusage);
			pfree(es.str->data);
		}
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ProcessUtility hook
 */
static void
pgsp_ProcessUtility(Node *parsetree, const char *queryString,
					ProcessUtilityContext context, ParamListInfo params,
					DestReceiver *dest, char *completionTag)
{
	if (prev_ProcessUtility)
		prev_ProcessUtility(parsetree, queryString,
							context, params,
							dest, completionTag);
	else
		standard_ProcessUtility(parsetree, queryString,
								context, params,
								dest, completionTag);
}

/*
 * Calculate hash value for a key
 */
static uint32
hash_table_fn(const void *key, Size keysize)
{
	const EntryKey *k = (const EntryKey *) key;

	/* we don't bother to include encoding in the hash */
	return hash_uint32((uint32) k->userid) ^
		hash_uint32((uint32) k->dbid) ^
		hash_uint32((uint32) k->queryid) ^
		hash_uint32((uint32) k->planid);
}

/*
 * Compare two keys - zero means match
 */
static int
match_fn(const void *key1, const void *key2, Size keysize)
{
	const EntryKey *k1 = (const EntryKey *) key1;
	const EntryKey *k2 = (const EntryKey *) key2;

	if (k1->userid == k2->userid &&
		k1->dbid == k2->dbid &&
		k1->encoding == k2->encoding &&
		k1->queryid == k2->queryid &&
		k1->planid == k2->planid)
		return 0;
	else
		return 1;
}

static uint32
hash_query(const char* query)
{
	uint32 queryid;

	char *normquery = pstrdup(query);
	normalize_expr(normquery, false);
	queryid = hash_any((const unsigned char*)normquery, strlen(normquery));
	elog(LOG, "NORMALIZED: %u: %s", queryid, normquery);
	pfree(normquery);

	return queryid;
}


/*
 * Store some statistics for a statement.
 *
 * If jstate is not NULL then we're trying to create an entry for which
 * we have no statistics as yet; we just want to record the normalized
 * query string.  total_time, rows, bufusage are ignored in this case.
 */
static void
store_entry(char *plan, uint32 queryId, uint32 queryId2,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage)
{
	EntryKey key;
	StatEntry  *entry;
	char	   *norm_query = NULL;
	int 		plan_len;
	char	   *normalized_plan = NULL;
	char	   *shorten_plan = NULL;
	volatile StatEntry *e;

	Assert(plan != NULL);

	/* Safety check... */
	if (!shared_state || !hash_table)
		return;

	/* Set up key for hashtable search */
	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.encoding = GetDatabaseEncoding();
	key.queryid = queryId;

	normalized_plan = pgsp_json_normalize(plan);
	shorten_plan = pgsp_json_shorten(plan);
	//elog(LOG, "Normalized: %s", normalized_plan);
	//elog(LOG, "Shorten: %s", shorten_plan);
	//elog(LOG, "Original: %s", plan);
	plan_len = strlen(shorten_plan);

	key.planid = hash_any((const unsigned char *)normalized_plan,
						  strlen(normalized_plan));
	pfree(normalized_plan);

	if (plan_len >= shared_state->plan_size)
		plan_len = pg_encoding_mbcliplen(GetDatabaseEncoding(),
										 shorten_plan,
										 plan_len,
										 shared_state->plan_size - 1);

	
	/* Lookup the hash table entry with shared lock. */
	LWLockAcquire(shared_state->lock, LW_SHARED);

	entry = (StatEntry *) hash_search(hash_table, &key, HASH_FIND, NULL);

	/* Create new entry, if not present */
	if (!entry)
	{
		/*
		 * We'll need exclusive lock to make a new entry.  There is no point
		 * in holding shared lock while we normalize the string, though.
		 */
		LWLockRelease(shared_state->lock);

		/* Acquire exclusive lock as required by entry_alloc() */
		LWLockAcquire(shared_state->lock, LW_EXCLUSIVE);

		entry = entry_alloc(&key, "", 0, false);
	}

	/* Increment the counts, except when jstate is not NULL */
	/*
	 * Grab the spinlock while updating the counters (see comment about
	 * locking rules at the head of the file)
	 */
	
	e = (volatile StatEntry *) entry;
	SpinLockAcquire(&e->mutex);
	
	e->queryid = queryId2;

	/* "Unstick" entry if it was previously sticky */
	if (e->counters.calls == 0)
	{
		e->counters.usage = USAGE_INIT;
		e->counters.first_call = GetCurrentTimestamp();
	}
	
	e->counters.calls += 1;
	e->counters.total_time += total_time;
	e->counters.rows += rows;
	e->counters.shared_blks_hit += bufusage->shared_blks_hit;
	e->counters.shared_blks_read += bufusage->shared_blks_read;
	e->counters.shared_blks_dirtied += bufusage->shared_blks_dirtied;
	e->counters.shared_blks_written += bufusage->shared_blks_written;
	e->counters.local_blks_hit += bufusage->local_blks_hit;
	e->counters.local_blks_read += bufusage->local_blks_read;
	e->counters.local_blks_dirtied += bufusage->local_blks_dirtied;
	e->counters.local_blks_written += bufusage->local_blks_written;
	e->counters.temp_blks_read += bufusage->temp_blks_read;
	e->counters.temp_blks_written += bufusage->temp_blks_written;
	e->counters.blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->blk_read_time);
	e->counters.blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->blk_write_time);
	e->counters.last_call = GetCurrentTimestamp();
	e->counters.usage += USAGE_EXEC(total_time);

	Assert(plan_len >= 0 && plan_len < shared_state->plan_size);
	memcpy(entry->plan, shorten_plan, plan_len);
	entry->plan_len = plan_len;
	entry->plan[plan_len] = '\0';
	
	SpinLockRelease(&e->mutex);

	LWLockRelease(shared_state->lock);

	/* We postpone this pfree until we're out of the lock */
	if (norm_query)
		pfree(norm_query);
}

/*
 * Reset all statement statistics.
 */
Datum
pg_store_plans_reset(PG_FUNCTION_ARGS)
{
	if (!shared_state || !hash_table)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_plan must be loaded via shared_preload_libraries")));
	entry_reset();
	PG_RETURN_VOID();
}

#define PG_STORE_PLANS_COLS			23

/*
 * Retrieve statement statistics.
 */
Datum
pg_store_plans(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Oid			userid = GetUserId();
	bool		is_superuser = superuser();
	HASH_SEQ_STATUS hash_seq;
	StatEntry  *entry;

	if (!shared_state || !hash_table)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_store_plans must be loaded via shared_preload_libraries")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	LWLockAcquire(shared_state->lock, LW_SHARED);

	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PG_STORE_PLANS_COLS];
		bool		nulls[PG_STORE_PLANS_COLS];
		int			i = 0;
		int64		queryid      = entry->key.queryid;
		int64		queryid_stmt = entry->queryid;
		int64		planid       = entry->key.planid;
		Counters	tmp;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		values[i++] = Int64GetDatumFast(queryid);
		values[i++] = Int64GetDatumFast(planid);

		values[i++] = Int64GetDatumFast(queryid_stmt);

		if (is_superuser || entry->key.userid == userid)
		{
			char	   *pstr = entry->plan;
			char	   *estr;

			switch (plan_format)
			{
			case PLAN_FORMAT_TEXT:
				pstr = pgsp_json_textize(entry->plan);
				break;
			case PLAN_FORMAT_JSON:
				pstr = pgsp_json_inflate(entry->plan);
				break;
			case PLAN_FORMAT_YAML:
				pstr = pgsp_json_yamlize(entry->plan);
				break;
			case PLAN_FORMAT_XML:
				pstr = pgsp_json_xmlize(entry->plan);
				break;
			default:
				break;
			}
			
			estr = (char *)
				pg_do_encoding_conversion((unsigned char *) pstr,
										  entry->plan_len,
										  entry->key.encoding,
										  GetDatabaseEncoding());
			values[i++] = CStringGetTextDatum(estr);

			if (estr != pstr)
				pfree(estr);
			if (pstr != entry->plan)
				pfree(pstr);
			
		}
		else
			values[i++] = CStringGetTextDatum("<insufficient privilege>");

		/* copy counters to a local variable to keep locking time short */
		{
			volatile StatEntry *e = (volatile StatEntry *) entry;

			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}

		/* Skip entry if unexecuted (ie, it's a pending "sticky" entry) */
		if (tmp.calls == 0)
			continue;

		values[i++] = Int64GetDatumFast(tmp.calls);
		values[i++] = Float8GetDatumFast(tmp.total_time);
		values[i++] = Int64GetDatumFast(tmp.rows);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_read);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_written);
		values[i++] = Int64GetDatumFast(tmp.local_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.local_blks_read);
		values[i++] = Int64GetDatumFast(tmp.local_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.local_blks_written);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_read);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_written);
		values[i++] = Float8GetDatumFast(tmp.blk_read_time);
		values[i++] = Float8GetDatumFast(tmp.blk_write_time);
		values[i++] = TimestampTzGetDatum(tmp.first_call);
		values[i++] = TimestampTzGetDatum(tmp.last_call);
		Assert(i == PG_STORE_PLANS_COLS);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	LWLockRelease(shared_state->lock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * Estimate shared memory space needed.
 */
static Size
shared_mem_size(void)
{
	Size		size;
	Size		entrysize;

	size = MAXALIGN(sizeof(SharedState));
	entrysize = offsetof(StatEntry, plan) +  pg_store_plan_size;
	size = add_size(size, hash_estimate_size(store_size, entrysize));

	return size;
}

/*
 * Allocate a new hashtable entry.
 * caller must hold an exclusive lock on shared_state->lock
 *
 * "query" need not be null-terminated; we rely on plan_len instead
 *
 * If "sticky" is true, make the new entry artificially sticky so that it will
 * probably still be there when the query finishes execution.  We do this by
 * giving it a median usage value rather than the normal value.  (Strictly
 * speaking, query strings are normalized on a best effort basis, though it
 * would be difficult to demonstrate this even under artificial conditions.)
 *
 * Note: despite needing exclusive lock, it's not an error for the target
 * entry to already exist.	This is because store_entry releases and
 * reacquires lock after failing to find a match; so someone else could
 * have made the entry while we waited to get exclusive lock.
 */
static StatEntry *
entry_alloc(EntryKey *key, const char *plan, int plan_len, bool sticky)
{
	StatEntry  *entry;
	bool		found;

	/* Make space if needed */
	while (hash_get_num_entries(hash_table) >= store_size)
		entry_dealloc();

	/* Find or create an entry with desired hash code */
	entry = (StatEntry *) hash_search(hash_table, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it */

		/* reset the statistics */
		memset(&entry->counters, 0, sizeof(Counters));
		/* set the appropriate initial usage count */
		entry->counters.usage = sticky ? shared_state->cur_median_usage : USAGE_INIT;
		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
		/* ... and don't forget the query text */
		Assert(plan_len >= 0 && plan_len < shared_state->plan_size);
		entry->plan_len = plan_len;
		memcpy(entry->plan, plan, plan_len);
		entry->plan[plan_len] = '\0';
	}

	return entry;
}

/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = (*(StatEntry *const *) lhs)->counters.usage;
	double		r_usage = (*(StatEntry *const *) rhs)->counters.usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}

/*
 * Deallocate least used entries.
 * Caller must hold an exclusive lock on shared_state->lock.
 */
static void
entry_dealloc(void)
{
	HASH_SEQ_STATUS hash_seq;
	StatEntry **entries;
	StatEntry  *entry;
	int			nvictims;
	int			i;

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values.
	 */

	entries = palloc(hash_get_num_entries(hash_table) * sizeof(StatEntry *));

	i = 0;
	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
		/* "Sticky" entries get a different usage decay rate. */
		if (entry->counters.calls == 0)
			entry->counters.usage *= STICKY_DECREASE_FACTOR;
		else
			entry->counters.usage *= USAGE_DECREASE_FACTOR;
	}

	qsort(entries, i, sizeof(StatEntry *), entry_cmp);

	/* Also, record the (approximate) median usage */
	if (i > 0)
		shared_state->cur_median_usage = entries[i / 2]->counters.usage;

	nvictims = Max(10, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(hash_table, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);
}

/*
 * Release all entries.
 */
static void
entry_reset(void)
{
	HASH_SEQ_STATUS hash_seq;
	StatEntry  *entry;

	LWLockAcquire(shared_state->lock, LW_EXCLUSIVE);

	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(hash_table, &entry->key, HASH_REMOVE, NULL);
	}

	LWLockRelease(shared_state->lock);
}

Datum
pg_store_plans_hash_query(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(hash_query(text_to_cstring(PG_GETARG_TEXT_P(0))));
}
		
Datum
pg_store_plans_shorten(PG_FUNCTION_ARGS)
{
	text *short_plan = PG_GETARG_TEXT_P(0);
	char *cjson = text_to_cstring(short_plan);
	char *cshorten = pgsp_json_shorten(cjson);
	PG_RETURN_TEXT_P(cstring_to_text(cshorten));
}

Datum
pg_store_plans_normalize(PG_FUNCTION_ARGS)
{
	text *short_plan = PG_GETARG_TEXT_P(0);
	char *cjson = text_to_cstring(short_plan);
	char *cnormalized = pgsp_json_normalize(cjson);
	PG_RETURN_TEXT_P(cstring_to_text(cnormalized));
}

Datum
pg_store_plans_jsonplan(PG_FUNCTION_ARGS)
{
	text *short_plan = PG_GETARG_TEXT_P(0);
	char *cshort = text_to_cstring(short_plan);
	char *cinflated = pgsp_json_inflate(cshort);
	PG_RETURN_TEXT_P(cstring_to_text(cinflated));
}

Datum
pg_store_plans_textplan(PG_FUNCTION_ARGS)
{
	text *short_plan = PG_GETARG_TEXT_P(0);
	char *cshort = text_to_cstring(short_plan);
	char *ctextized = pgsp_json_textize(cshort);

	PG_RETURN_TEXT_P(cstring_to_text(ctextized));
}

Datum
pg_store_plans_yamlplan(PG_FUNCTION_ARGS)
{
	text *short_plan = PG_GETARG_TEXT_P(0);
	char *cshort = text_to_cstring(short_plan);
	char *cyamlized = pgsp_json_yamlize(cshort);

	PG_RETURN_TEXT_P(cstring_to_text(cyamlized));
}

Datum
pg_store_plans_xmlplan(PG_FUNCTION_ARGS)
{
	text *short_plan = PG_GETARG_TEXT_P(0);
	char *cshort = text_to_cstring(short_plan);
	char *cxmlized = pgsp_json_xmlize(cshort);

	PG_RETURN_TEXT_P(cstring_to_text(cxmlized));
}
