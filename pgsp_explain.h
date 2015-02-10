void pgspExplainTriggers(ExplainState *es, QueryDesc *queryDesc);

/* ExplainInitState() is replaced with NewExlainState() in 9.5 */
#if PG_VERSION_NUM < 90500
ExplainState *NewExplainState(void);
#endif
