# pg_stat_plan/Makefile

MODULE_big = pg_store_plans
OBJS = pg_store_plans.o pgsp_json.o pgsp_json_text.o pgsp_explain.o

EXTENSION = pg_store_plans
DATA = pg_store_plans--1.0.sql

REGRESS = all
REGRESS_OPTS = --temp-config=regress.conf
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_store_plans
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

## Theses entries needs running server
DBNAME = postgres
testfiles: all.out all.sql

all.out: all.sql
	psql $(DBNAME) -a -q -f all.sql > all.out

all.sql: makeplanfile.sql json2sql.pl
	psql $(DBNAME) -f makeplanfile.sql | ./json2sql.pl > all.sql

clean-testfiles:
	rm -f all.out all.sql

deploy-testfiles: testfiles
	mv all.sql sql/
	mv all.out expected/
