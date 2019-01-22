# pg_stat_plan/Makefile

MODULES = pg_store_plans
STOREPLANSVER = 1.3

MODULE_big = pg_store_plans
OBJS = pg_store_plans.o pgsp_json.o pgsp_json_text.o pgsp_explain.o

EXTENSION = pg_store_plans
DATA = pg_store_plans--*.sql

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

STARBALL11 = pg_store_plans11-$(STOREPLANSVER).tar.gz
STARBALL10 = pg_store_plans10-$(STOREPLANSVER).tar.gz
STARBALL96 = pg_store_plans96-$(STOREPLANSVER).tar.gz
STARBALL95 = pg_store_plans95-$(STOREPLANSVER).tar.gz
STARBALL94 = pg_store_plans94-$(STOREPLANSVER).tar.gz
STARBALLS = $(STARBALL94) $(STARBALL95) $(STARBALL96) $(STARBALL10) $(STARBALL11)

TARSOURCES = Makefile *.c  *.h \
	pg_store_plans--*.sql \
	pg_store_plans.control \
	doc/* expected/*.out sql/*.sql \

LDFLAGS+=-Wl,--build-id

## These entries need running server
DBNAME = postgres

rpms: rpm94 rpm95 rpm96 rpm10 rpm11

$(STARBALLS): $(TARSOURCES)
	if [ -h $(subst .tar.gz,,$@) ]; then rm $(subst .tar.gz,,$@); fi
	if [ -e $(subst .tar.gz,,$@) ]; then \
	  echo "$(subst .tar.gz,,$@) is not a symlink. Stop."; \
	  exit 1; \
	fi
	ln -s . $(subst .tar.gz,,$@)
	tar -chzf $@ $(addprefix $(subst .tar.gz,,$@)/, $^)
	rm $(subst .tar.gz,,$@)

rpm94: $(STARBALL94)
	MAKE_ROOT=`pwd` rpmbuild -bb SPECS/pg_store_plans94.spec

rpm95: $(STARBALL95)
	MAKE_ROOT=`pwd` rpmbuild -bb SPECS/pg_store_plans95.spec

rpm96: $(STARBALL96)
	MAKE_ROOT=`pwd` rpmbuild -bb SPECS/pg_store_plans96.spec

rpm10: $(STARBALL10)
	MAKE_ROOT=`pwd` rpmbuild -bb SPECS/pg_store_plans10.spec

rpm11: $(STARBALL11)
	MAKE_ROOT=`pwd` rpmbuild -bb SPECS/pg_store_plans11.spec

testfiles: convert.out convert.sql

convert.out: convert.sql
	psql $(DBNAME) -a -q -f convert.sql > $@

convert.sql: makeplanfile.sql json2sql.pl
	psql $(DBNAME) -f makeplanfile.sql |& ./json2sql.pl > $@

clean-testfiles:
	rm -f convert.out convert.sql

deploy-testfiles: testfiles
	mv convert.sql sql/
	mv convert.out expected/
