/*-------------------------------------------------------------------------
 *
 * pgsp_json.c: Plan handler for JSON/XML/YAML style plans
 *
 * Copyright (c) 2012-2024, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plans/pgsp_json.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/* In PG16, include/scan.h was gone. Define required symbols manually.. */
/* must be in sync with src/backend/parser/gram.h */
#if PG_VERSION_NUM < 160000
#error This file should only be included for PostgreSQL 16 and above
#elif PG_VERSION_NUM < 170000
enum pgsptokentype
{
    IDENT = 258,                   /* IDENT  */
    FCONST = 260,                  /* FCONST  */
    SCONST = 261,                  /* SCONST  */
    BCONST = 263,                  /* BCONST  */
    XCONST = 264,                  /* XCONST  */
    Op = 265,                      /* Op  */
    ICONST = 266,                  /* ICONST  */
    CURRENT_CATALOG = 358,         /* CURRENT_CATALOG  */
    CURRENT_DATE = 359,            /* CURRENT_DATE  */
    CURRENT_ROLE = 360,            /* CURRENT_ROLE  */
    CURRENT_SCHEMA = 361,          /* CURRENT_SCHEMA  */
    CURRENT_TIME = 362,            /* CURRENT_TIME  */
    CURRENT_TIMESTAMP = 363,       /* CURRENT_TIMESTAMP  */
    CURRENT_USER = 364,            /* CURRENT_USER  */
    FALSE_P = 415,                 /* FALSE_P  */
    LOCALTIME = 502,               /* LOCALTIME  */
    LOCALTIMESTAMP = 503,          /* LOCALTIMESTAMP  */
    NULL_P = 540,                  /* NULL_P  */
    TRUE_P = 689,                  /* TRUE_P  */
};
#elif PG_VERSION_NUM < 180000
enum pgsptokentype
{
    IDENT = 258,                   /* IDENT  */
    FCONST = 260,                  /* FCONST  */
    SCONST = 261,                  /* SCONST  */
    BCONST = 263,                  /* BCONST  */
    XCONST = 264,                  /* XCONST  */
    Op = 265,                      /* Op  */
    ICONST = 266,                  /* ICONST  */
    CURRENT_CATALOG = 359,         /* CURRENT_CATALOG  */
    CURRENT_DATE = 360,            /* CURRENT_DATE  */
    CURRENT_ROLE = 361,            /* CURRENT_ROLE  */
    CURRENT_SCHEMA = 362,          /* CURRENT_SCHEMA  */
    CURRENT_TIME = 363,            /* CURRENT_TIME  */
    CURRENT_TIMESTAMP = 364,       /* CURRENT_TIMESTAMP  */
    CURRENT_USER = 365,            /* CURRENT_USER  */
    FALSE_P = 418,                 /* FALSE_P  */
    LOCALTIME = 512,               /* LOCALTIME  */
    LOCALTIMESTAMP = 513,          /* LOCALTIMESTAMP  */
    NULL_P = 552,                  /* NULL_P  */
    TRUE_P = 708,                  /* TRUE_P  */
};
#else
#error This version of PostgeSQL is not supported
#endif
