# pg_stat_plan/Makefile

MODULES = pg_store_plans
STOREPLANSVER = 1.6

MODULE_big = pg_store_plans
OBJS = pg_store_plans.o pgsp_json.o pgsp_json_text.o pgsp_explain.o

EXTENSION = pg_store_plans

PG_VERSION := $(shell pg_config --version | sed "s/^PostgreSQL //" | sed "s/\.[0-9]*$$//")

DATA = pg_store_plans--1.6.sql

REGRESS = convert store
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

STARBALL14 = pg_store_plans14-$(STOREPLANSVER).tar.gz
STARBALLS = $(STARBALL14)

TARSOURCES = Makefile *.c  *.h \
	pg_store_plans--*.sql \
	pg_store_plans.control \
	docs/* expected/*.out sql/*.sql \

ifneq ($(shell uname), SunOS)
LDFLAGS+=-Wl,--build-id
endif

## These entries need running server
DBNAME = postgres

rpms: rpm14

$(STARBALLS): $(TARSOURCES)
	if [ -h $(subst .tar.gz,,$@) ]; then rm $(subst .tar.gz,,$@); fi
	if [ -e $(subst .tar.gz,,$@) ]; then \
	  echo "$(subst .tar.gz,,$@) is not a symlink. Stop."; \
	  exit 1; \
	fi
	ln -s . $(subst .tar.gz,,$@)
	tar -chzf $@ $(addprefix $(subst .tar.gz,,$@)/, $^)
	rm $(subst .tar.gz,,$@)

rpm14: $(STARBALL14)
	MAKE_ROOT=`pwd` rpmbuild -bb SPECS/pg_store_plans14.spec

testfiles: convert.out convert.sql

convert.out: convert.sql
	psql $(DBNAME) -a -q -X -f convert.sql > $@

convert.sql: makeplanfile.sql json2sql.pl
	psql $(DBNAME) -X -f makeplanfile.sql |& ./json2sql.pl > $@

clean-testfiles:
	rm -f convert.out convert.sql

deploy-testfiles: testfiles
	mv convert.sql sql/
	mv convert.out expected/
