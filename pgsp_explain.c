/*-------------------------------------------------------------------------
 *
 * pgsp_explain.c: extracted code from explain.c for explain of triggers.
 *
 * Copyright (c) 2008-2020, PostgreSQL Global Development Group
 * Copyright (c) 2012-2021, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plans/pgsp_explain.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "commands/explain.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/json.h"
#include "pgsp_explain.h"

static void pgspExplainOpenGroup(const char *objtype, const char *labelname,
					  bool labeled, ExplainState *es);
static void pgspExplainCloseGroup(const char *objtype, const char *labelname,
					   bool labeled, ExplainState *es);
static void report_triggers(ResultRelInfo *rInfo, bool show_relname,
					 ExplainState *es);
static void pgspExplainPropertyText(const char *qlabel, const char *value, ExplainState *es);
static void pgspExplainPropertyFloat(const char *qlabel, double value, int ndigits,
						  ExplainState *es);
static void pgspExplainProperty(const char *qlabel, const char *value, bool numeric,
							ExplainState *es);
static void pgspExplainJSONLineEnding(ExplainState *es);

/*
 * ExplainState is modified at 9.4.1 and 9.3.6. But the change is for
 * internal use and to avoid binary-incompatibility not changing the
 * size of ExplainState. So we can use ExplainState->extra as if it
 * were grouping_stack safely and should do so. Using ->extra as List*
 * discards the memory for ExplainStateExtra but it is not a problem
 * since it is allocated by palloc.
 */
#if (PG_VERSION_NUM >= 90401 && PG_VERSION_NUM < 90500) || \
	(PG_VERSION_NUM >= 90306 && PG_VERSION_NUM < 90400)
#define GROUPING_STACK(es) (*((List **)(&(es)->extra)))
#else
#define GROUPING_STACK(es) ((es)->grouping_stack)
#endif

/* ExplainInitState() is replaced with NewExlainState() in 9.5 */
#if PG_VERSION_NUM < 90500
ExplainState *
NewExplainState(void)
{
  ExplainState *es = (ExplainState *)palloc0(sizeof(ExplainState));

  ExplainInitState(es);
  es->costs = true;
  return es;
}
#endif

void
pgspExplainTriggers(ExplainState *es, QueryDesc *queryDesc)
{
	if (es->analyze)
	{
		ResultRelInfo *rInfo;
		bool		show_relname;
#if PG_VERSION_NUM < 140000
		int         numrels = queryDesc->estate->es_num_result_relations;
		int         nr;
#else
		List       *resultrels;
		List       *routerels;
#endif
		List	   *targrels = queryDesc->estate->es_trig_target_relations;
		ListCell   *l;

#if PG_VERSION_NUM >= 140000
		resultrels = queryDesc->estate->es_opened_result_relations;
		routerels = queryDesc->estate->es_tuple_routing_result_relations;
		targrels = queryDesc->estate->es_trig_target_relations;
#endif

		pgspExplainOpenGroup("Triggers", "Triggers", false, es);

#if PG_VERSION_NUM < 140000
		show_relname = (numrels > 1 || targrels != NIL);
		rInfo = queryDesc->estate->es_result_relations;
		for (nr = 0; nr < numrels; rInfo++, nr++)
#else
		show_relname = (list_length(resultrels) > 1 ||
						routerels != NIL || targrels != NIL);
		foreach(l, resultrels)
		{
			rInfo = (ResultRelInfo *) lfirst(l);
#endif
			report_triggers(rInfo, show_relname, es);
#if PG_VERSION_NUM >= 140000
		}

		foreach(l, routerels)
		{
			rInfo = (ResultRelInfo *) lfirst(l);
			report_triggers(rInfo, show_relname, es);
		}
#endif

		foreach(l, targrels)
		{
			rInfo = (ResultRelInfo *) lfirst(l);
			report_triggers(rInfo, show_relname, es);
		}

		pgspExplainCloseGroup("Triggers", "Triggers", false, es);
	}
}

static void
pgspExplainOpenGroup(const char *objtype, const char *labelname,
				 bool labeled, ExplainState *es)
{
	pgspExplainJSONLineEnding(es);
	appendStringInfoSpaces(es->str, 2 * es->indent);
	if (labelname)
	{
		escape_json(es->str, labelname);
		appendStringInfoString(es->str, ": ");
	}
	appendStringInfoChar(es->str, labeled ? '{' : '[');

	GROUPING_STACK(es) = lcons_int(0, GROUPING_STACK(es));
	es->indent++;
}

static void
pgspExplainCloseGroup(const char *objtype, const char *labelname,
				  bool labeled, ExplainState *es)
{
	es->indent--;
	appendStringInfoChar(es->str, '\n');
	appendStringInfoSpaces(es->str, 2 * es->indent);
	appendStringInfoChar(es->str, labeled ? '}' : ']');
	GROUPING_STACK(es) = list_delete_first(GROUPING_STACK(es));
}

static void
report_triggers(ResultRelInfo *rInfo, bool show_relname, ExplainState *es)
{
	int			nt;

	if (!rInfo->ri_TrigDesc || !rInfo->ri_TrigInstrument)
		return;
	for (nt = 0; nt < rInfo->ri_TrigDesc->numtriggers; nt++)
	{
		Trigger    *trig = rInfo->ri_TrigDesc->triggers + nt;
		Instrumentation *instr = rInfo->ri_TrigInstrument + nt;
		char	   *relname;
		char	   *conname = NULL;

		/* Must clean up instrumentation state */
		InstrEndLoop(instr);

		/*
		 * We ignore triggers that were never invoked; they likely aren't
		 * relevant to the current query type.
		 */
		if (instr->ntuples == 0)
			continue;

		pgspExplainOpenGroup("Trigger", NULL, true, es);

		relname = RelationGetRelationName(rInfo->ri_RelationDesc);
		if (OidIsValid(trig->tgconstraint))
			conname = get_constraint_name(trig->tgconstraint);

		pgspExplainPropertyText("Trigger Name", trig->tgname, es);
		if (conname)
			pgspExplainPropertyText("Constraint Name", conname, es);
		pgspExplainPropertyText("Relation", relname, es);
		pgspExplainPropertyFloat("Time", 1000.0 * instr->total, 3, es);
		pgspExplainPropertyFloat("Calls", instr->ntuples, 0, es);

		if (conname)
			pfree(conname);

		pgspExplainCloseGroup("Trigger", NULL, true, es);
	}
}

static void
pgspExplainPropertyText(const char *qlabel, const char *value, ExplainState *es)
{
	pgspExplainProperty(qlabel, value, false, es);
}

static void
pgspExplainPropertyFloat(const char *qlabel, double value, int ndigits,
					 ExplainState *es)
{
	char		buf[256];

	snprintf(buf, sizeof(buf), "%.*f", ndigits, value);
	pgspExplainProperty(qlabel, buf, true, es);
}


static void
pgspExplainProperty(const char *qlabel, const char *value, bool numeric,
				ExplainState *es)
{
	pgspExplainJSONLineEnding(es);
	appendStringInfoSpaces(es->str, es->indent * 2);
	escape_json(es->str, qlabel);
	appendStringInfoString(es->str, ": ");
	if (numeric)
		appendStringInfoString(es->str, value);
	else
		escape_json(es->str, value);
}

static void
pgspExplainJSONLineEnding(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_JSON);
	if (linitial_int(GROUPING_STACK(es)) != 0)
		appendStringInfoChar(es->str, ',');
	else
		linitial_int(GROUPING_STACK(es)) = 1;
	appendStringInfoChar(es->str, '\n');
}

