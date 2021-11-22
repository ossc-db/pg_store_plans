/*-------------------------------------------------------------------------
 *
 * pg_store_plans.c
 *		Take statistics of plan selection across a whole database cluster.
 *
 * Execution costs are totaled for each distinct plan for each query,
 * and plan and queryid are kept in a shared hashtable, each record in
 * which is associated with a record in pg_stat_statements, if any, by
 * the queryid.
 *
 * For Postgres 9.3 or earlier does not expose query id so
 * pg_store_plans needs to calculate it based on the given query
 * string using different algorithm from pg_stat_statements, and later
 * the id will be matched against the one made from query string
 * stored in pg_stat_statements. For the reason, queryid matching in
 * this way will fail if the query string kept in pg_stat_statements
 * is truncated in the middle.
 *
 * Plans are identified by fingerprinting plan representations in
 * "shortened" JSON format with constants and unstable values such as
 * rows, width, loops ignored. Nevertheless, stored plan entries hold
 * them of the latest execution. Entry eviction is done in the same
 * way to pg_stat_statements.
 *
 * Copyright (c) 2008-2020, PostgreSQL Global Development Group
 * Copyright (c) 2012-2021, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plans/pg_store_plans.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#include "catalog/pg_authid.h"
#include "commands/explain.h"
#include "access/hash.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/spin.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#if PG_VERSION_NUM >= 140000
#include "utils/queryjumble.h"
#endif
#include "utils/timestamp.h"

#include "pgsp_json.h"
#include "pgsp_explain.h"

PG_MODULE_MAGIC;

/* Location of stats file */
#define PGSP_DUMP_FILE	"global/pg_store_plans.stat"
#define PGSP_TEXT_FILE	PG_STAT_TMP_DIR "/pgsp_plan_texts.stat"

/* PostgreSQL major version number, changes in which invalidate all entries */
static const uint32 PGSP_PG_MAJOR_VERSION = PG_VERSION_NUM / 100;

/* This constant defines the magic number in the stats file header */
static const uint32 PGSP_FILE_HEADER = 0x20211125;
static int max_plan_len = 5000;

/* XXX: Should USAGE_EXEC reflect execution time and/or buffer usage? */
#define USAGE_EXEC(duration)	(1.0)
#define USAGE_INIT				(1.0)	/* including initial planning */
#define ASSUMED_MEDIAN_INIT		(10.0)	/* initial assumed median usage */
#define ASSUMED_LENGTH_INIT		1024	/* initial assumed mean query length */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every entry_dealloc */
#define STICKY_DECREASE_FACTOR	(0.50)	/* factor for sticky entries */
#define USAGE_DEALLOC_PERCENT	5		/* free this % of entries at once */

/* In PostgreSQL 11, queryid becomes a uint64 internally.
 */
#if PG_VERSION_NUM >= 110000
typedef uint64 queryid_t;
#define PGSP_NO_QUERYID		UINT64CONST(0)
#else
typedef uint32 queryid_t;
#define PGSP_NO_QUERYID		0
#endif

/*
 * Extension version number, for supporting older extension versions' objects
 */
typedef enum pgspVersion
{
	PGSP_V1_5 = 0,
	PGSP_V1_6
} pgspVersion;

/*
 * Hashtable key that defines the identity of a hashtable entry.  We separate
 * queries by user and by database even if they are otherwise identical.
 *
 * Presently, the query encoding is fully determined by the source database
 * and so we don't really need it to be in the key.  But that might not always
 * be true. Anyway it's notationally convenient to pass it as part of the key.
 */
typedef struct pgspHashKey
{
	Oid			userid;			/* user OID */
	Oid			dbid;			/* database OID */
	queryid_t	queryid;		/* query identifier */
	uint32		planid;			/* plan identifier */
} pgspHashKey;

/*
 * The actual stats counters kept within pgspEntry.
 */
typedef struct Counters
{
	int64		calls;				/* # of times executed */
	double		total_time;			/* total execution time, in msec */
	double		min_time;			/* minimum execution time in msec */
	double		max_time;			/* maximum execution time in msec */
	double		mean_time;			/* mean execution time in msec */
	double		sum_var_time;	/* sum of variances in execution time in msec */
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
 * Global statistics for pg_store_plans
 */
typedef struct pgspGlobalStats
{
	int64		dealloc;		/* # of times entries were deallocated */
	TimestampTz stats_reset;	/* timestamp with all stats reset */
} pgspGlobalStats;

/*
 * Statistics per plan
 *
 * NB: see the file read/write code before changing field order here.
 */
typedef struct pgspEntry
{
	pgspHashKey	key;			/* hash key of entry - MUST BE FIRST */
	Counters	counters;		/* the statistics for this query */
	Size		plan_offset;	/* plan text offset in extern file */
	int			plan_len;		/* # of valid bytes in query string */
	int			encoding;		/* query encoding */
	slock_t		mutex;			/* protects the counters only */
} pgspEntry;

/*
 * Global shared state
 */
typedef struct pgspSharedState
{
	LWLock	   *lock;			/* protects hashtable search/modification */
	int			plan_size;		/* max query length in bytes */
	double		cur_median_usage;	/* current median usage in hashtable */
	Size		mean_plan_len;	/* current mean entry text length */
	slock_t		mutex;			/* protects following fields only: */
	Size		extent;			/* current extent of plan file */
	int			n_writers;		/* number of active writers to query file */
	int			gc_count;		/* plan file garbage collection cycle count */
	pgspGlobalStats stats;		/* global statistics for pgsp */
} pgspSharedState;

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
static pgspSharedState *shared_state = NULL;
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

/* options for plan storage */
typedef enum
{
	PLAN_STORAGE_SHMEM,		/* plan is stored as a part of hash entry */
	PLAN_STORAGE_FILE		/* plan is stored in a separate file */
}  pgspPlanStorage;

static const struct config_enum_entry plan_storage_options[] =
{
	{"shmem", PLAN_STORAGE_SHMEM, false},
	{"file", PLAN_STORAGE_FILE, false},
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
static int  plan_format;		/* Plan representation style in
								 * pg_store_plans.plan  */
static int  plan_storage;		/* Plan storage type */

#if PG_VERSION_NUM >= 140000
/*
 * For pg14 and later, we rely on core queryid calculation.  If
 * it's not available it means that the admin explicitly refused to
 * compute it, for performance reason or other.  In that case, we
 * will also consider that this extension is disabled.
 */
#define pgsp_enabled(q) \
	((track_level == TRACK_LEVEL_ALL || \
	(track_level == TRACK_LEVEL_TOP && nested_level == 0)) && \
	(q != PGSP_NO_QUERYID))
#else
#define pgsp_enabled(q) \
	(track_level == TRACK_LEVEL_ALL || \
	(track_level == TRACK_LEVEL_TOP && nested_level == 0))
#endif

#define SHMEM_PLAN_PTR(ent) (((char *) ent) + sizeof(pgspEntry))

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
Datum		pg_store_plans_info(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_store_plans_reset);
PG_FUNCTION_INFO_V1(pg_store_plans_hash_query);
PG_FUNCTION_INFO_V1(pg_store_plans);
PG_FUNCTION_INFO_V1(pg_store_plans_1_6);
PG_FUNCTION_INFO_V1(pg_store_plans_shorten);
PG_FUNCTION_INFO_V1(pg_store_plans_normalize);
PG_FUNCTION_INFO_V1(pg_store_plans_jsonplan);
PG_FUNCTION_INFO_V1(pg_store_plans_yamlplan);
PG_FUNCTION_INFO_V1(pg_store_plans_xmlplan);
PG_FUNCTION_INFO_V1(pg_store_plans_textplan);
PG_FUNCTION_INFO_V1(pg_store_plans_info);

#if PG_VERSION_NUM < 130000
#define COMPTAG_TYPE char
#else
#define COMPTAG_TYPE QueryCompletion
#endif

#if PG_VERSION_NUM < 140000
#define ROLE_PG_READ_ALL_STATS		DEFAULT_ROLE_READ_ALL_STATS
#endif

static void pgsp_shmem_startup(void);
static void pgsp_shmem_shutdown(int code, Datum arg);
static void pgsp_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgsp_ExecutorRun(QueryDesc *queryDesc,
				 ScanDirection direction,
							 uint64 count, bool execute_once);
static void pgsp_ExecutorFinish(QueryDesc *queryDesc);
static void pgsp_ExecutorEnd(QueryDesc *queryDesc);
static void pgsp_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
#if PG_VERSION_NUM >= 140000
					bool readOnlyTree,
#endif
					ProcessUtilityContext context, ParamListInfo params,
					QueryEnvironment *queryEnv,
					DestReceiver *dest, COMPTAG_TYPE *completionTag);
static uint32 hash_query(const char* query);
static void pgsp_store(char *plan, queryid_t queryId,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage);
static void pg_store_plans_internal(FunctionCallInfo fcinfo,
									pgspVersion api_version);
static Size shared_mem_size(void);
static pgspEntry *entry_alloc(pgspHashKey *key, Size plan_offset, int plan_len,
							  bool sticky);
static bool ptext_store(const char *plan, int plan_len, Size *plan_offset,
						int *gc_count);
static char *ptext_load_file(Size *buffer_size);
static char *ptext_fetch(Size plan_offset, int plan_len, char *buffer,
						 Size buffer_size);
static bool need_gc_ptexts(void);
static void gc_ptexts(void);
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

#if PG_VERSION_NUM >= 140000
	/*
	 * Inform the postmaster that we want to enable query_id calculation if
	 * compute_query_id is set to auto.
	 */
	EnableQueryId();
#endif

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

	DefineCustomIntVariable("pg_store_plans.max_plan_length",
	  "Sets the maximum length of plans stored by pg_store_plans.",
							NULL,
							&max_plan_len,
							5000,
							100,
							INT32_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("pg_store_plans.plan_storage",
			   "Selects where to store plan texts.",
							 NULL,
							 &plan_storage,
							 PLAN_STORAGE_FILE,
							 plan_storage_options,
							 PGC_USERSET,
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
	RequestNamedLWLockTranche("pg_store_plans", 1);

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
	FILE	   *pfile = NULL;
	uint32		header;
	int32		num;
	int32		pgver;
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
						   sizeof(pgspSharedState),
						   &found);

	if (!found)
	{
		/* First time through ... */
		shared_state->lock = &(GetNamedLWLockTranche("pg_store_plans"))->lock;
		shared_state->plan_size = max_plan_len;
		shared_state->cur_median_usage = ASSUMED_MEDIAN_INIT;
		shared_state->mean_plan_len = ASSUMED_LENGTH_INIT;
		SpinLockInit(&shared_state->mutex);
		shared_state->extent = 0;
		shared_state->n_writers = 0;
		shared_state->gc_count = 0;
		shared_state->stats.dealloc = 0;
		shared_state->stats.stats_reset = GetCurrentTimestamp();
	}

	/* Be sure everyone agrees on the hash table entry size */
	plan_size = shared_state->plan_size;

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(pgspHashKey);
	info.entrysize = sizeof(pgspEntry);
	if (plan_storage == PLAN_STORAGE_SHMEM)
		info.entrysize += max_plan_len;
	hash_table = ShmemInitHash("pg_store_plans hash",
							  store_size, store_size,
							  &info, HASH_ELEM |
							  HASH_BLOBS);

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(pgsp_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 */
	if (found)
		return;

	/*
	 * Note: we don't bother with locks here, because there should be no other
	 * processes running when this code is reached.
	 */

	/* Unlink query text file possibly left over from crash */
	unlink(PGSP_TEXT_FILE);

	if (plan_storage == PLAN_STORAGE_FILE)
	{
		/* Allocate new query text temp file */
		pfile = AllocateFile(PGSP_TEXT_FILE, PG_BINARY_W);
		if (pfile == NULL)
			goto write_error;
	}

	/*
	 * If we were told not to load old statistics, we're done.  (Note we do
	 * not try to unlink any old dump file in this case.  This seems a bit
	 * questionable but it's the historical behavior.)
	 */
	if (!dump_on_shutdown)
	{
		if (pfile)
			FreeFile(pfile);
		return;
	}

	/*
	 * Attempt to load old statistics from the dump file.
	 */
	file = AllocateFile(PGSP_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno == ENOENT)
			return;				/* ignore not-found error */
		/* No existing persisted stats file, so we're done */
		goto read_error;
	}

	buffer_size = plan_size;
	buffer = (char *) palloc(buffer_size);

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&pgver, sizeof(uint32), 1, file) != 1 ||
		fread(&num, sizeof(int32), 1, file) != 1)
		goto read_error;

	if (header != PGSP_FILE_HEADER ||
		pgver != PGSP_PG_MAJOR_VERSION)
		goto data_error;

	for (i = 0; i < num; i++)
	{
		pgspEntry	temp;
		pgspEntry  *entry;
		Size		plan_offset = 0;

		if (fread(&temp, sizeof(pgspEntry), 1, file) != 1)
			goto read_error;

		/* Encoding is the only field we can easily sanity-check */
		if (!PG_VALID_BE_ENCODING(temp.encoding))
			goto data_error;

		/* Previous incarnation might have had a larger plan_size */
		if (temp.plan_len >= buffer_size)
		{
			buffer = (char *) repalloc(buffer, temp.plan_len + 1);
			buffer_size = temp.plan_len + 1;
		}

		if (fread(buffer, 1, temp.plan_len + 1, file) != temp.plan_len + 1)
			goto read_error;

		/* Skip loading "sticky" entries */
		if (temp.counters.calls == 0)
			continue;

		/* Clip to available length if needed */
		if (temp.plan_len >= plan_size)
			temp.plan_len = pg_encoding_mbcliplen(temp.encoding,
												   buffer,
												   temp.plan_len,
												   plan_size - 1);

		buffer[temp.plan_len] = '\0';

		if (plan_storage == PLAN_STORAGE_FILE)
		{
			/* Store the plan text */
			plan_offset = shared_state->extent;
			if (fwrite(buffer, 1, temp.plan_len + 1, pfile) !=
				temp.plan_len + 1)
				goto write_error;
			shared_state->extent += temp.plan_len + 1;
		}

		/* make the hashtable entry (discards old entries if too many) */
		entry = entry_alloc(&temp.key, plan_offset, temp.plan_len, false);

		if (plan_storage == PLAN_STORAGE_SHMEM)
			memcpy(SHMEM_PLAN_PTR(entry), buffer, temp.plan_len + 1);

		/* copy in the actual stats */
		entry->counters = temp.counters;
	}

	pfree(buffer);
	FreeFile(file);

	if (pfile)
		FreeFile(pfile);

	/*
	 * Remove the file so it's not included in backups/replication slaves,
	 * etc. A new file will be written on next shutdown.
	 */
	unlink(PGSP_DUMP_FILE);

	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read file \"%s\": %m",
					PGSP_DUMP_FILE)));
	goto fail;
data_error:
	ereport(LOG,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("ignoring invalid data in file \"%s\"",
					PGSP_DUMP_FILE)));
	goto fail;
write_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSP_TEXT_FILE)));
fail:
	if (buffer)
		pfree(buffer);
	if (file)
		FreeFile(file);
	if (pfile)
		FreeFile(pfile);
	/* If possible, throw away the bogus file; ignore any error */
	unlink(PGSP_DUMP_FILE);

	/*
	 * Don't unlink PGSP_TEXT_FILE here; it should always be around while the
	 * server is running with pg_stat_statements enabled
	 */
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
	char	   *pbuffer = NULL;
	Size		pbuffer_size = 0;
	HASH_SEQ_STATUS hash_seq;
	int32		num_entries;
	pgspEntry  *entry;

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
	if (fwrite(&PGSP_PG_MAJOR_VERSION, sizeof(uint32), 1, file) != 1)
		goto error;
	num_entries = hash_get_num_entries(hash_table);
	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;

	if (plan_storage == PLAN_STORAGE_FILE)
	{
		pbuffer = ptext_load_file(&pbuffer_size);
		if (pbuffer == NULL)
			goto error;
	}

	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			len = entry->plan_len;
		char	   *pstr;

		if (plan_storage == PLAN_STORAGE_FILE)
			pstr = ptext_fetch(entry->plan_offset, len,
							   pbuffer, pbuffer_size);
		else
			pstr = SHMEM_PLAN_PTR(entry);

		if (pstr == NULL)
			continue;			/* Ignore any entries with bogus texts */

		if (fwrite(entry, sizeof(pgspEntry), 1, file) != 1 ||
			fwrite(pstr, 1, len + 1, file) != len + 1)
		{
			/* note: we assume hash_seq_term won't change errno */
			hash_seq_term(&hash_seq);
			goto error;
		}
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

	/* Unlink query-texts file; it's not needed while shutdown */
	unlink(PGSP_TEXT_FILE);

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
	if (queryDesc->totaltime == NULL &&
			pgsp_enabled(queryDesc->plannedstmt->queryId))
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
		queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL
#if PG_VERSION_NUM >= 140000
										  , false
#endif
										 );
		MemoryContextSwitchTo(oldcxt);
	}

}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgsp_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
				 bool execute_once)
{
	nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
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
		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);

		if (pgsp_enabled(queryDesc->plannedstmt->queryId) &&
			queryDesc->totaltime->total &&
			queryDesc->totaltime->total >=
			(double)min_duration / 1000.0)
		{
			queryid_t	  queryid;
			ExplainState *es;
			StringInfo	  es_str;

			es = NewExplainState();
			es_str = es->str;

			es->analyze = queryDesc->instrument_options;
			es->verbose = log_verbose;
			es->buffers = (es->analyze && log_buffers);
			es->timing = (es->analyze && log_timing);
			es->format = EXPLAIN_FORMAT_JSON;

			ExplainBeginOutput(es);
			ExplainPrintPlan(es, queryDesc);
			if (log_triggers)
				pgspExplainTriggers(es, queryDesc);
			ExplainEndOutput(es);

			/* Remove last line break */
			if (es_str->len > 0 && es_str->data[es_str->len - 1] == '\n')
				es_str->data[--es_str->len] = '\0';

			/* JSON outmost braces. */
			es_str->data[0] = '{';
			es_str->data[es_str->len - 1] = '}';

			queryid = queryDesc->plannedstmt->queryId;
#if PG_VERSION_NUM < 140000
			/*
			 * For versions before pg14, a queryid is only available if
			 * pg_stat_statements extension (or similar) if configured.  We
			 * don't want a hard requirement for such an extension so fallback
			 * to an internal queryid calculation in some case.
			 * For pg14 and above, core postgres can compute a queryid so we
			 * will rely on it.
			 */
			if (queryid == PGSP_NO_QUERYID)
				queryid = (queryid_t) hash_query(queryDesc->sourceText);
#else
			Assert(queryid != PGSP_NO_QUERYID);
#endif

			pgsp_store(es_str->data,
						queryid,
						queryDesc->totaltime->total * 1000.0,	/* convert to msec */
						queryDesc->estate->es_processed,
						&queryDesc->totaltime->bufusage);
			pfree(es_str->data);
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
pgsp_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
#if PG_VERSION_NUM >= 140000
					bool readOnlyTree,
#endif
					ProcessUtilityContext context, ParamListInfo params,
					QueryEnvironment *queryEnv,
					DestReceiver *dest, COMPTAG_TYPE *completionTag)
{
	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
							readOnlyTree,
#endif
							context, params, queryEnv,
							dest, completionTag);
	else
		standard_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
								readOnlyTree,
#endif
								context, params, queryEnv,
								dest, completionTag);
}

/*
 * hash_query: calculate internal query ID for a query
 *
 *  As of PG11, Query.queryId has been widen to 64 bit to reduce collision of
 *  queries to practical level. On the other hand pg_store_plans uses the
 *  combination of query hash and plan hash values as the hash table key and
 *  the resolution of the hash value effectively has the same degree so we
 *  continue to use uint32 as internal queryid.
 *
 *  This may merge plans from different queries into single internal query id
 *  but it is not a problem when pg_stat_statements is used together since the
 *  extension gives enough resolution on queries.
 */
static uint32
hash_query(const char* query)
{
	uint32 queryid;

	char *normquery = pstrdup(query);
	normalize_expr(normquery, false);
	queryid = hash_any((const unsigned char*)normquery, strlen(normquery));
	pfree(normquery);

	/* If we are unlucky enough to get a hash of zero, use 1 instead */
	if (queryid == 0)
		queryid = 1;

	return queryid;
}


/*
 * Store some statistics for a plan.
 *
 * Table entry is keyed with userid.dbid.queryId.planId. planId is the hash
 * value of the given plan, which is calculated in ths function.
 */
static void
pgsp_store(char *plan, queryid_t queryId,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage)
{
	pgspHashKey key;
	pgspEntry  *entry;
	char	   *norm_query = NULL;
	int 		plan_len;
	char	   *normalized_plan = NULL;
	char	   *shorten_plan = NULL;
	volatile pgspEntry *e;
	Size		plan_offset = 0;
	bool		do_gc = false;

	Assert(plan != NULL && queryId != PGSP_NO_QUERYID);

	/* Safety check... */
	if (!shared_state || !hash_table)
		return;

	/* Set up key for hashtable search */
	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.queryid = queryId;

	normalized_plan = pgsp_json_normalize(plan);
	shorten_plan = pgsp_json_shorten(plan);
	elog(DEBUG3, "pg_store_plans: Normalized plan: %s", normalized_plan);
	elog(DEBUG3, "pg_store_plans: Shorten plan: %s", shorten_plan);
	elog(DEBUG3, "pg_store_plans: Original plan: %s", plan);
	plan_len = strlen(shorten_plan);

	key.planid = hash_any((const unsigned char *)normalized_plan,
						  strlen(normalized_plan));
	pfree(normalized_plan);

	if (plan_len >= shared_state->plan_size)
		plan_len = pg_encoding_mbcliplen(GetDatabaseEncoding(),
										 shorten_plan,
										 plan_len,
										 shared_state->plan_size - 1);


	/* Look up the hash table entry with shared lock. */
	LWLockAcquire(shared_state->lock, LW_SHARED);

	entry = (pgspEntry *) hash_search(hash_table, &key, HASH_FIND, NULL);

	/* Store the plan text, if the entry not present */
	if (!entry && plan_storage == PLAN_STORAGE_FILE)
	{
		int		gc_count;
		bool	stored;

		/* Append new plan text to file with only shared lock held */
		stored = ptext_store(shorten_plan, plan_len, &plan_offset, &gc_count);

		/*
		 * Determine whether we need to garbage collect external query texts
		 * while the shared lock is still held.  This micro-optimization
		 * avoids taking the time to decide this while holding exclusive lock.
		 */
		do_gc = need_gc_ptexts();

		/* Acquire exclusive lock as required by entry_alloc() */
		LWLockRelease(shared_state->lock);
		LWLockAcquire(shared_state->lock, LW_EXCLUSIVE);

		/*
		 * A garbage collection may have occurred while we weren't holding the
		 * lock.  In the unlikely event that this happens, the plan text we
		 * stored above will have been garbage collected, so write it again.
		 * This should be infrequent enough that doing it while holding
		 * exclusive lock isn't a performance problem.
		 */
		if (!stored || shared_state->gc_count != gc_count)
			stored = ptext_store(shorten_plan, plan_len, &plan_offset, NULL);

		/* If we failed to write to the text file, give up */
		if (!stored)
			goto done;

	}

	/* Create new entry, if not present */
	if (!entry)
	{
		entry = entry_alloc(&key, plan_offset, plan_len, false);

		/* shorten_plan is terminated by NUL */
		if (plan_storage == PLAN_STORAGE_SHMEM)
			memcpy(SHMEM_PLAN_PTR(entry), shorten_plan, plan_len + 1);
			

		/* If needed, perform garbage collection while exclusive lock held */
		if (do_gc)
			gc_ptexts();
	}

	/* Increment the counts, except when jstate is not NULL */

	/*
	 * Grab the spinlock while updating the counters (see comment about
	 * locking rules at the head of the file)
	 */

	e = (volatile pgspEntry *) entry;
	SpinLockAcquire(&e->mutex);

	/* "Unstick" entry if it was previously sticky */
	if (e->counters.calls == 0)
	{
		e->counters.usage = USAGE_INIT;
		e->counters.first_call = GetCurrentTimestamp();
	}

	e->counters.calls += 1;
	e->counters.total_time += total_time;
	if (e->counters.calls == 1)
	{
		e->counters.min_time = total_time;
		e->counters.max_time = total_time;
		e->counters.mean_time = total_time;
	}
	else
	{
		/*
		 * Welford's method for accurately computing variance. See
		 * <http://www.johndcook.com/blog/standard_deviation/>
		 */
		double		old_mean = e->counters.mean_time;

		e->counters.mean_time +=
			(total_time - old_mean) / e->counters.calls;
		e->counters.sum_var_time +=
			(total_time - old_mean) * (total_time - e->counters.mean_time);

		/* calculate min and max time */
		if (e->counters.min_time > total_time)
			e->counters.min_time = total_time;
		if (e->counters.max_time < total_time)
			e->counters.max_time = total_time;
	}

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

	SpinLockRelease(&e->mutex);

done:
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
				 errmsg("pg_store_plans must be loaded via shared_preload_libraries")));
	entry_reset();
	PG_RETURN_VOID();
}

/* Number of output arguments (columns) for various API versions */
#define PG_STORE_PLANS_COLS_V1_5	27
#define PG_STORE_PLANS_COLS_V1_6	26
#define PG_STORE_PLANS_COLS			27	/* maximum of above */

/*
 * Retrieve statement statistics.
 */
Datum
pg_store_plans_1_6(PG_FUNCTION_ARGS)
{
	pg_store_plans_internal(fcinfo, PGSP_V1_6);

	return (Datum) 0;
}

Datum
pg_store_plans(PG_FUNCTION_ARGS)
{
	pg_store_plans_internal(fcinfo, PGSP_V1_5);

	return (Datum) 0;
}

static void
pg_store_plans_internal(FunctionCallInfo fcinfo,
						pgspVersion api_version)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Oid			userid = GetUserId();
	bool		is_allowed_role = is_member_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS);
	int			n_writers;
	char	   *pbuffer = NULL;
	Size		pbuffer_size = 0;
	Size		extent = 0;
	int			gc_count = 0;
	HASH_SEQ_STATUS hash_seq;
	pgspEntry  *entry;

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

	/*
	 * We'd like to load the plan text file (if needed) while not holding any
	 * lock on shared_state->lock.  In the worst case we'll have to do this
	 * again after we have the lock, but it's unlikely enough to make this a
	 * win despite occasional duplicated work.  We need to reload if anybody
	 * writes to the file (either a retail ptext_store(), or a garbage
	 * collection) between this point and where we've gotten shared lock.  If a
	 * ptext_store is actually in progress when we look, we might as well skip
	 * the speculative load entirely.
	 */

	/* Take the mutex so we can examine variables */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) shared_state;

		SpinLockAcquire(&s->mutex);
		extent = s->extent;
		n_writers = s->n_writers;
		gc_count = s->gc_count;
		SpinLockRelease(&s->mutex);
	}

	/* No point in loading file now if there are active writers */
	if (n_writers == 0 && plan_storage == PLAN_STORAGE_FILE)
		pbuffer = ptext_load_file(&pbuffer_size);

	/*
	 * Get shared lock, load or reload the plan text file if we must, and
	 * iterate over the hashtable entries.
	 *
	 * With a large hash table, we might be holding the lock rather longer
	 * than one could wish.  However, this only blocks creation of new hash
	 * table entries, and the larger the hash table the less likely that is to
	 * be needed.  So we can hope this is okay.  Perhaps someday we'll decide
	 * we need to partition the hash table to limit the time spent holding any
	 * one lock.
	 */
	LWLockAcquire(shared_state->lock, LW_SHARED);

	/*
	 * Here it is safe to examine extent and gc_count without taking the mutex.
	 * Note that although other processes might change shared_state->extent
	 * just after we look at it, the strings they then write into the file
	 * cannot yet be referenced in the hashtable, so we don't care whether we
	 * see them or not.
	 *
	 * If ptext_load_file fails, we just press on; we'll return NULL for every
	 * plan text.
	 */
	if (plan_storage == PLAN_STORAGE_FILE &&
		(pbuffer == NULL ||
		 shared_state->extent != extent ||
		 shared_state->gc_count != gc_count))
	{
		if (pbuffer)
			free(pbuffer);
		pbuffer = ptext_load_file(&pbuffer_size);
	}

	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PG_STORE_PLANS_COLS];
		bool		nulls[PG_STORE_PLANS_COLS];
		int			i = 0;
		int64		queryid      = entry->key.queryid;
		int64		planid       = entry->key.planid;
		Counters	tmp;
		double		stddev;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		if (is_allowed_role || entry->key.userid == userid)
		{
			values[i++] = Int64GetDatumFast(queryid);
			values[i++] = Int64GetDatumFast(planid);
			if (api_version == PGSP_V1_5)
				values[i++] = ObjectIdGetDatum(queryid);
		}
		else
		{
			values[i++] = Int64GetDatumFast(0);
			values[i++] = Int64GetDatumFast(0);
			if (api_version == PGSP_V1_5)
				values[i++] = Int64GetDatumFast(0);
		}

		if (is_allowed_role || entry->key.userid == userid)
		{
			char	   *pstr; /* Plan string */
			char	   *mstr; /* Modified plan string */
			char	   *estr; /* Encoded modified plan string */

			if (plan_storage == PLAN_STORAGE_FILE)
				pstr = ptext_fetch(entry->plan_offset, entry->plan_len,
								   pbuffer, pbuffer_size);
			else
				pstr = SHMEM_PLAN_PTR(entry);

			switch (plan_format)
			{
				case PLAN_FORMAT_TEXT:
					mstr = pgsp_json_textize(pstr);
					break;
				case PLAN_FORMAT_JSON:
					mstr = pgsp_json_inflate(pstr);
					break;
				case PLAN_FORMAT_YAML:
					mstr = pgsp_json_yamlize(pstr);
					break;
				case PLAN_FORMAT_XML:
					mstr = pgsp_json_xmlize(pstr);
					break;
				default:
					break;
			}

			estr = (char *)
				pg_do_encoding_conversion((unsigned char *) mstr,
										  strlen(mstr),
										  entry->encoding,
										  GetDatabaseEncoding());
			values[i++] = CStringGetTextDatum(estr);

			if (estr != mstr)
				pfree(estr);

			if (mstr != pstr)
				pfree(mstr);

			/* pstr is a pointer onto pbuffer */
		}
		else
			values[i++] = CStringGetTextDatum("<insufficient privilege>");

		/* copy counters to a local variable to keep locking time short */
		{
			volatile pgspEntry *e = (volatile pgspEntry *) entry;

			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}

		/* Skip entry if unexecuted (ie, it's a pending "sticky" entry) */
		if (tmp.calls == 0)
			continue;

		values[i++] = Int64GetDatumFast(tmp.calls);
		values[i++] = Float8GetDatumFast(tmp.total_time);
		values[i++] = Float8GetDatumFast(tmp.min_time);
		values[i++] = Float8GetDatumFast(tmp.max_time);
		values[i++] = Float8GetDatumFast(tmp.mean_time);

		/*
		 * Note we are calculating the population variance here, not the
		 * sample variance, as we have data for the whole population, so
		 * Bessel's correction is not used, and we don't divide by
		 * tmp.calls - 1.
		 */
		if (tmp.calls > 1)
			stddev = sqrt(tmp.sum_var_time / tmp.calls);
		else
			stddev = 0.0;
		values[i++] = Float8GetDatumFast(stddev);

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

		Assert(i == (api_version == PGSP_V1_5 ? PG_STORE_PLANS_COLS_V1_5 :
					 api_version == PGSP_V1_6 ? PG_STORE_PLANS_COLS_V1_6 :
					 -1 /* fail if you forget to update this assert */ ));

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	LWLockRelease(shared_state->lock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);
}

/* Number of output arguments (columns) for pg_stat_statements_info */
#define PG_STORE_PLANS_INFO_COLS	2

/*
 * Return statistics of pg_stat_statements.
 */
Datum
pg_store_plans_info(PG_FUNCTION_ARGS)
{
	pgspGlobalStats stats;
	TupleDesc	tupdesc;
	Datum		values[PG_STORE_PLANS_INFO_COLS];
	bool		nulls[PG_STORE_PLANS_INFO_COLS];

	if (!shared_state || !hash_table)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_store_plans must be loaded via shared_preload_libraries")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Read global statistics for pg_stat_statements */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) shared_state;

		SpinLockAcquire(&s->mutex);
		stats = s->stats;
		SpinLockRelease(&s->mutex);
	}

	values[0] = Int64GetDatum(stats.dealloc);
	values[1] = TimestampTzGetDatum(stats.stats_reset);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Estimate shared memory space needed.
 */
static Size
shared_mem_size(void)
{
	Size		size;
	int			entry_size;

	size = MAXALIGN(sizeof(pgspSharedState));
	entry_size = sizeof(pgspEntry);

	/* plan text is apppended to the struct body */
	if (plan_storage == PLAN_STORAGE_SHMEM)
		entry_size += max_plan_len;

	size = add_size(size, hash_estimate_size(store_size, entry_size));

	return size;
}

/*
 * Allocate a new hashtable entry.
 * caller must hold an exclusive lock on shared_state->lock
 *
 * "plan" need not be null-terminated; we rely on plan_len instead
 *
 * If "sticky" is true, make the new entry artificially sticky so that it will
 * probably still be there when the query finishes execution.  We do this by
 * giving it a median usage value rather than the normal value.  (Strictly
 * speaking, query strings are normalized on a best effort basis, though it
 * would be difficult to demonstrate this even under artificial conditions.)
 *
 * Note: despite needing exclusive lock, it's not an error for the target
 * entry to already exist.	This is because pgsp_store releases and
 * reacquires lock after failing to find a match; so someone else could
 * have made the entry while we waited to get exclusive lock.
 */
static pgspEntry *
entry_alloc(pgspHashKey *key, Size plan_offset, int plan_len, bool sticky)
{
	pgspEntry  *entry;
	bool		found;

	/* Make space if needed */
	while (hash_get_num_entries(hash_table) >= store_size)
		entry_dealloc();

	/* Find or create an entry with desired hash code */
	entry = (pgspEntry *) hash_search(hash_table, key, HASH_ENTER, &found);

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
		entry->plan_offset = plan_offset;
		entry->plan_len = plan_len;
		entry->encoding = GetDatabaseEncoding();
	}

	return entry;
}

/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = (*(pgspEntry *const *) lhs)->counters.usage;
	double		r_usage = (*(pgspEntry *const *) rhs)->counters.usage;

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
	pgspEntry **entries;
	pgspEntry  *entry;
	int			nvictims;
	int			i;
	Size		tottextlen;
	int			nvalidtexts;

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values.
	 */

	entries = palloc(hash_get_num_entries(hash_table) * sizeof(pgspEntry *));

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

		/* In the mean length computation, ignore dropped texts. */
		if (entry->plan_len >= 0)
		{
			tottextlen += entry->plan_len + 1;
			nvalidtexts++;
		}
	}

	qsort(entries, i, sizeof(pgspEntry *), entry_cmp);

	/* Also, record the (approximate) median usage */
	if (i > 0)
		shared_state->cur_median_usage = entries[i / 2]->counters.usage;
	/* Record the mean plan length */
	if (nvalidtexts > 0)
		shared_state->mean_plan_len = tottextlen / nvalidtexts;
	else
		shared_state->mean_plan_len = ASSUMED_LENGTH_INIT;

	nvictims = Max(10, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(hash_table, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);

	/* Increment the number of times entries are deallocated */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) shared_state;

		SpinLockAcquire(&s->mutex);
		s->stats.dealloc += 1;
		SpinLockRelease(&s->mutex);
	}
}

/*
 * Given a plan string (not necessarily null-terminated), allocate a new
 * entry in the external plan text file and store the string there.
 *
 * If successful, returns true, and stores the new entry's offset in the file
 * into *plan_offset.  Also, if gc_count isn't NULL, *gc_count is set to the
 * number of garbage collections that have occurred so far.
 *
 * On failure, returns false.
 *
 * At least a shared lock on shared_state->lock must be held by the caller, so
 * as to prevent a concurrent garbage collection.  Share-lock-holding callers
 * should pass a gc_count pointer to obtain the number of garbage collections,
 * so that they can recheck the count after obtaining exclusive lock to detect
 * whether a garbage collection occurred (and removed this entry).
 */
static bool
ptext_store(const char *plan, int plan_len, Size *plan_offset, int *gc_count)
{
	Size		off;
	int			fd;

	Assert (plan_storage == PLAN_STORAGE_FILE);

	/*
	 * We use a spinlock to protect extent/n_writers/gc_count, so that
	 * multiple processes may execute this function concurrently.
	 */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) shared_state;

		SpinLockAcquire(&s->mutex);
		off = s->extent;
		s->extent += plan_len + 1;
		s->n_writers++;
		if (gc_count)
			*gc_count = s->gc_count;
		SpinLockRelease(&s->mutex);
	}

	*plan_offset = off;

	/* Now write the data into the successfully-reserved part of the file */
	fd = OpenTransientFile(PGSP_TEXT_FILE, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0)
		goto error;

	if (pg_pwrite(fd, plan, plan_len, off) != plan_len)
		goto error;
	if (pg_pwrite(fd, "\0", 1, off + plan_len) != 1)
		goto error;

	CloseTransientFile(fd);

	/* Mark our write complete */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) shared_state;

		SpinLockAcquire(&s->mutex);
		s->n_writers--;
		SpinLockRelease(&s->mutex);
	}

	return true;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSP_TEXT_FILE)));

	if (fd >= 0)
		CloseTransientFile(fd);

	/* Mark our write complete */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) shared_state;

		SpinLockAcquire(&s->mutex);
		s->n_writers--;
		SpinLockRelease(&s->mutex);
	}

	return false;
}

/*
 * Read the external plan text file into a malloc'd buffer.
 *
 * Returns NULL (without throwing an error) if unable to read, eg
 * file not there or insufficient memory.
 *
 * On success, the buffer size is also returned into *buffer_size.
 *
 * This can be called without any lock on shared_state->lock, but in that case
 * the caller is responsible for verifying that the result is sane.
 */
static char *
ptext_load_file(Size *buffer_size)
{
	char	   *buf;
	int			fd;
	struct stat stat;
	Size		nread;

	Assert (plan_storage == PLAN_STORAGE_FILE);

	fd = OpenTransientFile(PGSP_TEXT_FILE, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							PGSP_TEXT_FILE)));
		return NULL;
	}

	/* Get file length */
	if (fstat(fd, &stat))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						PGSP_TEXT_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/* Allocate buffer; beware that off_t might be wider than size_t */
	if (stat.st_size <= MaxAllocHugeSize)
		buf = (char *) malloc(stat.st_size);
	else
		buf = NULL;
	if (buf == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Could not allocate enough memory to read file \"%s\".",
						   PGSP_TEXT_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/*
	 * OK, slurp in the file.  Windows fails if we try to read more than
	 * INT_MAX bytes at once, and other platforms might not like that either,
	 * so read a very large file in 1GB segments.
	 */
	nread = 0;
	while (nread < stat.st_size)
	{
		int			toread = Min(1024 * 1024 * 1024, stat.st_size - nread);

		/*
		 * If we get a short read and errno doesn't get set, the reason is
		 * probably that garbage collection truncated the file since we did
		 * the fstat(), so we don't log a complaint --- but we don't return
		 * the data, either, since it's most likely corrupt due to concurrent
		 * writes from garbage collection.
		 */
		errno = 0;
		if (read(fd, buf + nread, toread) != toread)
		{
			if (errno)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m",
								PGSP_TEXT_FILE)));
			free(buf);
			CloseTransientFile(fd);
			return NULL;
		}
		nread += toread;
	}

	if (CloseTransientFile(fd) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", PGSP_TEXT_FILE)));

	*buffer_size = nread;
	return buf;
}

/*
 * Locate a plan text in the file image previously read by ptext_load_file().
 *
 * We validate the given offset/length, and return NULL if bogus.  Otherwise,
 * the result points to a null-terminated string within the buffer.
 */
static char *
ptext_fetch(Size plan_offset, int plan_len,
			char *buffer, Size buffer_size)
{
	Assert (plan_storage == PLAN_STORAGE_FILE);

	/* File read failed? */
	if (buffer == NULL)
		return NULL;
	/* Bogus offset/length? */
	if (plan_len < 0 ||
		plan_offset + plan_len >= buffer_size)
		return NULL;
	/* As a further sanity check, make sure there's a trailing null */
	if (buffer[plan_offset + plan_len] != '\0')
		return NULL;
	/* Looks OK */
	return buffer + plan_offset;
}

/*
 * Do we need to garbage-collect the external plan text file?
 *
 * Caller should hold at least a shared lock on shared_state->lock.
 */
static bool
need_gc_ptexts(void)
{
	Size		extent;

	Assert (plan_storage == PLAN_STORAGE_FILE);

	/* Read shared extent pointer */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) shared_state;

		SpinLockAcquire(&s->mutex);
		extent = s->extent;
		SpinLockRelease(&s->mutex);
	}

	/* Don't proceed if file does not exceed 512 bytes per possible entry */
	if (extent < 512 * store_size)
		return false;

	/*
	 * Don't proceed if file is less than about 50% bloat.  Nothing can or
	 * should be done in the event of unusually large query texts accounting
	 * for file's large size.  We go to the trouble of maintaining the mean
	 * query length in order to prevent garbage collection from thrashing
	 * uselessly.
	 */
	if (extent < shared_state->mean_plan_len * store_size * 2)
		return false;

	return true;
}

/*
 * Garbage-collect orphaned plan texts in external file.
 *
 * This won't be called often in the typical case, since it's likely that
 * there won't be too much churn, and besides, a similar compaction process
 * occurs when serializing to disk at shutdown or as part of resetting.
 * Despite this, it seems prudent to plan for the edge case where the file
 * becomes unreasonably large, with no other method of compaction likely to
 * occur in the foreseeable future.
 *
 * The caller must hold an exclusive lock on shared_state->lock.
 *
 * At the first sign of trouble we unlink the query text file to get a clean
 * slate (although existing statistics are retained), rather than risk
 * thrashing by allowing the same problem case to recur indefinitely.
 */
static void
gc_ptexts(void)
{
	char	   *pbuffer;
	Size		pbuffer_size;
	FILE	   *pfile = NULL;
	HASH_SEQ_STATUS hash_seq;
	pgspEntry  *entry;
	Size		extent;
	int			nentries;

	Assert (plan_storage == PLAN_STORAGE_FILE);

	/*
	 * When called from store_entry, some other session might have proceeded
	 * with garbage collection in the no-lock-held interim of lock strength
	 * escalation.  Check once more that this is actually necessary.
	 */
	if (!need_gc_ptexts())
		return;

	/*
	 * Load the old texts file.  If we fail (out of memory, for instance),
	 * invalidate query texts.  Hopefully this is rare.  It might seem better
	 * to leave things alone on an OOM failure, but the problem is that the
	 * file is only going to get bigger; hoping for a future non-OOM result is
	 * risky and can easily lead to complete denial of service.
	 */
	pbuffer = ptext_load_file(&pbuffer_size);
	if (pbuffer == NULL)
		goto gc_fail;

	/*
	 * We overwrite the plan texts file in place, so as to reduce the risk of
	 * an out-of-disk-space failure.  Since the file is guaranteed not to get
	 * larger, this should always work on traditional filesystems; though we
	 * could still lose on copy-on-write filesystems.
	 */
	pfile = AllocateFile(PGSP_TEXT_FILE, PG_BINARY_W);
	if (pfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PGSP_TEXT_FILE)));
		goto gc_fail;
	}

	extent = 0;
	nentries = 0;

	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			plan_len = entry->plan_len;
		char	   *plan = ptext_fetch(entry->plan_offset,
									   plan_len,
									   pbuffer,
									   pbuffer_size);

		if (plan == NULL)
		{
			/* Trouble ... drop the text */
			entry->plan_offset = 0;
			entry->plan_len = -1;
			/* entry will not be counted in mean plan length computation */
			continue;
		}

		if (fwrite(plan, 1, plan_len + 1, pfile) != plan_len + 1)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							PGSP_TEXT_FILE)));
			hash_seq_term(&hash_seq);
			goto gc_fail;
		}

		entry->plan_offset = extent;
		extent += plan_len + 1;
		nentries++;
	}

	/*
	 * Truncate away any now-unused space.  If this fails for some odd reason,
	 * we log it, but there's no need to fail.
	 */
	if (ftruncate(fileno(pfile), extent) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m",
						PGSP_TEXT_FILE)));

	if (FreeFile(pfile))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PGSP_TEXT_FILE)));
		pfile = NULL;
		goto gc_fail;
	}

	elog(DEBUG1, "pgsp gc of queries file shrunk size from %zu to %zu",
		 shared_state->extent, extent);

	/* Reset the shared extent pointer */
	shared_state->extent = extent;

	/*
	 * Also update the mean plan length, to be sure that need_gc_ptexts()
	 * won't still think we have a problem.
	 */
	if (nentries > 0)
		shared_state->mean_plan_len = extent / nentries;
	else
		shared_state->mean_plan_len = ASSUMED_LENGTH_INIT;

	free(pbuffer);

	return;

gc_fail:
	/* clean up resources */
	if (pfile)
		FreeFile(pfile);
	if (pbuffer)
		free(pbuffer);

	/*
	 * Since the contents of the external file are now uncertain, mark all
	 * hashtable entries as having invalid texts.
	 */
	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entry->plan_offset = 0;
		entry->plan_len = -1;
	}

	/*
	 * Destroy the query text file and create a new, empty one
	 */
	(void) unlink(PGSP_TEXT_FILE);
	pfile = AllocateFile(PGSP_TEXT_FILE, PG_BINARY_W);
	if (pfile == NULL)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not recreate file \"%s\": %m",
						PGSP_TEXT_FILE)));
	else
		FreeFile(pfile);

	/* Reset the shared extent pointer */
	shared_state->extent = 0;

	/* Reset mean_plan_len to match the new state */
	shared_state->mean_plan_len = ASSUMED_LENGTH_INIT;
}

/*
 * Release all entries.
 */
static void
entry_reset(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgspEntry  *entry;
	FILE	   *pfile;

	if (!shared_state || !hash_table)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_store_plans must be loaded via shared_preload_libraries")));

	LWLockAcquire(shared_state->lock, LW_EXCLUSIVE);

	hash_seq_init(&hash_seq, hash_table);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(hash_table, &entry->key, HASH_REMOVE, NULL);
	}

	/*
	 * Reset global statistics for pg_store_plans.
	 */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) shared_state;
		TimestampTz stats_reset = GetCurrentTimestamp();

		SpinLockAcquire(&s->mutex);
		s->stats.dealloc = 0;
		s->stats.stats_reset = stats_reset;
		SpinLockRelease(&s->mutex);
	}

	/*
	 * Write new empty plan file, perhaps even creating a new one to recover
	 * if the file was missing.
	 */
	pfile = AllocateFile(PGSP_TEXT_FILE, PG_BINARY_W);
	if (pfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						PGSP_TEXT_FILE)));
		goto done;
	}

	/* If ftruncate fails, log it, but it's not a fatal problem */
	if (ftruncate(fileno(pfile), 0) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m",
						PGSP_TEXT_FILE)));

	FreeFile(pfile);

done:
	shared_state->extent = 0;
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
