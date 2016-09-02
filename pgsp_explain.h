/*-------------------------------------------------------------------------
 *
 * pgsp_explain.h: extracted code from explain.c for explain of triggers.
 *
 * Copyright (c) 2008-2016, PostgreSQL Global Development Group
 * Copyright (c) 2012-2016, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plan/pgsp_explain.h
 *
 *-------------------------------------------------------------------------
 */

void pgspExplainTriggers(ExplainState *es, QueryDesc *queryDesc);

/* ExplainInitState() is replaced with NewExlainState() in 9.5 */
#if PG_VERSION_NUM < 90500
ExplainState *NewExplainState(void);
#endif
