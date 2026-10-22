# pg_plan_mgmt/Makefile

MODULE_big = pg_plan_mgmt
OBJS = \
	$(WIN32RES) \
	pg_plan_mgmt.o \
	pgpm_funcs.o \
	pgpm_persist.o \
	pgpm_planner.o \
	pgpm_store.o

EXTENSION = pg_plan_mgmt
DATA = pg_plan_mgmt--1.0.sql
PGFILEDESC = "pg_plan_mgmt - plan baselines built on pg_plan_advice"

REGRESS = capture enforce pruning functions environment
REGRESS_OPTS = --temp-config $(srcdir)/pg_plan_mgmt.conf
TAP_TESTS = 1

# pg_plan_advice is a PostgreSQL 19 contrib module; its header is installed
# under include/server/contrib/pg_plan_advice.
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_CPPFLAGS = -I$(shell $(PG_CONFIG) --includedir-server)/contrib/pg_plan_advice
include $(PGXS)

# The regression tests need pg_plan_advice and pg_plan_mgmt preloaded, which
# installcheck against an existing server can't arrange. Run them in a
# temporary instance built from the installed binaries instead ("make
# install" first).
.PHONY: localcheck
localcheck:
	$(pgxsdir)/src/test/regress/pg_regress \
		--bindir='$(bindir)' \
		--inputdir=$(srcdir) \
		--temp-instance=./tmp_check \
		$(REGRESS_OPTS) $(REGRESS)

.PHONY: tapcheck
tapcheck:
	rm -rf $(srcdir)/tmp_check
	PATH='$(bindir):$(PATH)' \
	PERL5LIB='$(pgxsdir)/src/test/perl:$(PERL5LIB)' \
	PG_REGRESS='$(pgxsdir)/src/test/regress/pg_regress' \
	prove -I $(pgxsdir)/src/test/perl $(srcdir)/t/*.pl
