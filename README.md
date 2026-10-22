# pg_plan_mgmt

[![CI](https://github.com/pganalyze/pg_plan_mgmt/actions/workflows/ci.yml/badge.svg)](https://github.com/pganalyze/pg_plan_mgmt/actions/workflows/ci.yml)

> **Experimental.** This is a first iteration. The SQL interface, GUCs and the
> on-disk format may still change, and it has not seen production use.

Plan baselines for PostgreSQL, built on `contrib/pg_plan_advice`, with no core
patches.

Every distinct plan the optimizer produces for a managed statement is
captured, identified by the advice string ("outline") pg_plan_advice
generates for it. The first plan of a statement is **Approved**, later ones
**Unapproved**. With baselines in use, the planner's minimum-cost plan runs
only if it is Approved or Preferred; otherwise the cheapest enforceable
**Preferred** plan runs, else the cheapest enforceable **Approved** one, else
the minimum-cost plan. Plans can also be **Rejected** or disabled.

## Requirements

PostgreSQL 19 or later, with `pg_plan_advice` installed.

## Installation

```sh
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
```

```
# postgresql.conf
shared_preload_libraries = 'pg_plan_advice,pg_plan_mgmt'
```

```sql
CREATE EXTENSION pg_plan_mgmt;   -- creates schema plan_mgmt
```

Loading via `shared_preload_libraries` is required for persistence and plan
retention. Without it (`session_preload_libraries`, `LOAD`) baselines live in
shared memory until restart.

## Usage

Capture plans, either for a known set of statements:

```sql
SET pg_plan_mgmt.capture_plan_baselines = manual;
EXPLAIN SELECT ...;          -- or run the statements
SET pg_plan_mgmt.capture_plan_baselines = off;
```

or for everything that is planned at least twice, with
`pg_plan_mgmt.capture_plan_baselines = automatic` in `postgresql.conf`.

Then enforce:

```sql
SET pg_plan_mgmt.use_plan_baselines = on;
EXPLAIN SELECT ...;
```

```
 Hash Join
   ...
 Supplied Plan Advice:
   JOIN_ORDER(f d) /* matched */
   ...
 Plan Management: Approved plan used instead of minimum-cost plan
 Plan Hash: 4127... (Approved), Minimum-Cost Plan Hash: -9011... (Unapproved)
```

Review and manage plans:

```sql
SELECT query_id, plan_hash, status, enabled, valid, sql_text, plan_outline,
       estimated_total_cost, last_used, plan_count
  FROM plan_mgmt.dba_plans;

SELECT plan_mgmt.set_plan_status(query_id, plan_hash, 'Approved');  -- or Preferred, Rejected, Unapproved
SELECT plan_mgmt.set_plan_enabled(query_id, plan_hash, false);
SELECT plan_mgmt.delete_plan(query_id, plan_hash);    -- plan_hash NULL: whole statement
SELECT * FROM plan_mgmt.validate_plans('update');     -- or 'disable', 'delete'
SELECT plan_mgmt.reset();                             -- everything in this database
```

To force a plan the optimizer doesn't choose on its own, capture it while
steering the planner with `pg_plan_advice.advice` and make it Preferred, or
add an outline directly (it is checked and canonicalized):

```sql
SELECT * FROM plan_mgmt.add_plan(
  'SELECT * FROM a JOIN b ON a.id = b.a_id WHERE a.x < $1',
  'JOIN_ORDER(a b) NESTED_LOOP_PLAIN(b) INDEX_SCAN(a a_pkey)',
  'Preferred');
```

`add_plan` computes the query ID from the statement text in the current
database, so exporting `sql_text, plan_outline, status, enabled` from
`dba_plans` and feeding the rows to `add_plan` moves baselines between
environments.

All functions and the view are revoked from PUBLIC; grant access as needed.

## Configuration

| GUC | default | |
|---|---|---|
| `pg_plan_mgmt.capture_plan_baselines` | `off` | `off`, `manual`, `automatic` (superuser) |
| `pg_plan_mgmt.use_plan_baselines` | `off` | restrict planning to approved plans |
| `pg_plan_mgmt.unapproved_plan_execution_threshold` | `0` | let unapproved plans cheaper than this run |
| `pg_plan_mgmt.max_plans` | `10000` | capture stops when reached |
| `pg_plan_mgmt.max_statements` | `10000` | capture stops when reached |
| `pg_plan_mgmt.plan_retention_period` | `32` | days; unused plans are deleted, 0 keeps them |
| `pg_plan_mgmt.persist` | `on` | save to `pg_plan_mgmt.tsv` in the data directory |
| `pg_plan_mgmt.persist_interval` | `30s` | 0 saves only at shutdown |

## Tests

```sh
make install
make localcheck   # regression tests in a temporary instance
make tapcheck     # persistence and retention
```

## License

Licensed under the PostgreSQL license<br>
Copyright (c) 2026, Duboce Labs, Inc. (pganalyze) <team@pganalyze.com>

See LICENSE file for details.
