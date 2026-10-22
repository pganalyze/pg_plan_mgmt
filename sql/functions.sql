-- Bad arguments.
SELECT plan_mgmt.set_plan_status(1, 1, 'bogus');
SELECT plan_mgmt.set_plan_status(1, 1, 'approved');
SELECT plan_mgmt.set_plan_enabled(1, 1, true);
SELECT plan_mgmt.delete_plan(1, 1);
SELECT * FROM plan_mgmt.validate_plans('bogus');
SELECT plan_mgmt.add_plan('SELECT 1; SELECT 2', 'SEQ_SCAN(x)');
SELECT plan_mgmt.add_plan('VACUUM pm_dim', 'SEQ_SCAN(pm_dim)');
SELECT plan_mgmt.add_plan('SELECT * FROM pm_dim', 'INDEX_SCAN(pm_dim no_such_idx)');
SELECT plan_mgmt.add_plan('SELECT * FROM pm_dim', 'not advice');

-- add_plan stores the regenerated, canonical outline; EXPLAIN is unwrapped.
SELECT plan_mgmt.add_plan('EXPLAIN (COSTS OFF) SELECT * FROM pm_dim WHERE id = $1',
						  'BITMAP_HEAP_SCAN(pm_dim)', 'unapproved')
  IS NOT NULL;
SELECT status, origin, sql_text, plan_outline FROM plan_mgmt.plans;

-- Adding the same plan again only updates it.
SELECT plan_mgmt.add_plan('SELECT * FROM pm_dim WHERE id = $1',
						  'BITMAP_HEAP_SCAN(pm_dim)', 'approved', false)
  IS NOT NULL;
SELECT status, enabled FROM plan_mgmt.plans;

-- A second plan, using an index we're about to drop.
CREATE INDEX pm_dim_val_idx ON pm_dim (val);
SELECT plan_mgmt.add_plan('SELECT * FROM pm_dim WHERE id = $1 AND val = $2',
						  'INDEX_SCAN(pm_dim pm_dim_val_idx)') IS NOT NULL;
SELECT valid, estimated_total_cost > 0 AS has_cost, detail
  FROM plan_mgmt.validate_plans() ORDER BY valid;
DROP INDEX pm_dim_val_idx;

-- validate_plans reports, then disables, then deletes the invalid plan.
SELECT valid, detail FROM plan_mgmt.validate_plans() ORDER BY valid;
SELECT valid, enabled, last_validated IS NOT NULL AS validated
  FROM plan_mgmt.plans ORDER BY valid;
SELECT valid, detail FROM plan_mgmt.validate_plans('disable') ORDER BY valid;
SELECT valid, enabled FROM plan_mgmt.plans ORDER BY valid;
SELECT valid, detail FROM plan_mgmt.validate_plans('delete') ORDER BY valid;
SELECT valid, enabled FROM plan_mgmt.plans ORDER BY valid;

-- A plan whose table is gone is invalid, not an error.
CREATE TABLE pm_gone (a int);
SELECT plan_mgmt.add_plan('SELECT * FROM pm_gone', 'SEQ_SCAN(pm_gone)') IS NOT NULL;
DROP TABLE pm_gone;
SELECT valid, detail FROM plan_mgmt.validate_plans() ORDER BY valid;

-- Deleting.
SELECT query_id AS qid, plan_no AS ph FROM plan_mgmt.plans
 WHERE sql_text LIKE '%pm_gone%' \gset
SELECT plan_mgmt.delete_plan(:qid, :ph);
SELECT plan_mgmt.delete_plan(:qid);
SELECT query_id AS qid FROM plan_mgmt.plans LIMIT 1 \gset
SELECT plan_mgmt.delete_plan(:qid);
SELECT count(*) FROM plan_mgmt.plans;

-- Statements planned inside PREPARE, DECLARE CURSOR, CREATE TABLE AS and
-- COPY are captured as submitted; validate_plans and add_plan unwrap them.
SET pg_plan_mgmt.capture_plan_baselines = manual;
PREPARE pm_prep(bigint) AS SELECT * FROM pm_dim WHERE id = $1;
EXECUTE pm_prep(1);
BEGIN;
DECLARE pm_cur SCROLL CURSOR FOR SELECT * FROM pm_dim WHERE val = 1;
FETCH 1 FROM pm_cur;
COMMIT;
CREATE TABLE pm_ctas AS SELECT id FROM pm_dim WHERE val = 2;
COPY (SELECT val FROM pm_dim WHERE id < 3) TO '/dev/null';
SET pg_plan_mgmt.capture_plan_baselines = off;
SELECT sql_text FROM plan_mgmt.plans ORDER BY sql_text;
SELECT valid, detail FROM plan_mgmt.validate_plans();
-- Re-adding each plan from its own text finds the same statement.
SELECT a.query_id = p.query_id AS same_query, a.plan_no = p.plan_no AS same_plan
  FROM plan_mgmt.plans p,
       LATERAL plan_mgmt.add_plan(p.sql_text, p.plan_outline, 'Preferred') a
 ORDER BY p.sql_text;
SELECT count(*), count(*) FILTER (WHERE status = 'Preferred') FROM plan_mgmt.plans;
-- The prepared statement's parameter types are part of its query ID.
SELECT plan_mgmt.add_plan('SELECT * FROM pm_dim WHERE id = $1', 'SEQ_SCAN(pm_dim)') IS NOT NULL;
SELECT count(*) FROM plan_mgmt.plans;
DEALLOCATE pm_prep;
DROP TABLE pm_ctas;
SELECT plan_mgmt.reset();

-- Functions are not available to unprivileged users by default.
CREATE ROLE regress_pm_user;
SET ROLE regress_pm_user;
SELECT * FROM plan_mgmt.plans;
SELECT plan_mgmt.reset();
RESET ROLE;
DROP ROLE regress_pm_user;
