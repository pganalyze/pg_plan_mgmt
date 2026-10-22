CREATE EXTENSION pg_plan_mgmt;

-- EXPLAIN without costs, with plan hashes masked.
CREATE FUNCTION explain_masked(query text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || query
	LOOP
		RETURN NEXT regexp_replace(ln, '-?\d{4,}', 'N', 'g');
	END LOOP;
END;
$$;

CREATE TABLE pm_dim (id int PRIMARY KEY, val int)
	WITH (autovacuum_enabled = false);
INSERT INTO pm_dim SELECT g, g % 10 FROM generate_series(1, 1000) g;
CREATE TABLE pm_fact (id int PRIMARY KEY, dim_id int)
	WITH (autovacuum_enabled = false);
INSERT INTO pm_fact SELECT g, g % 1000 + 1 FROM generate_series(1, 20000) g;
CREATE INDEX pm_fact_dim_id_idx ON pm_fact (dim_id);
VACUUM ANALYZE pm_dim, pm_fact;

-- Nothing is captured while capture is off.
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
SELECT count(*) FROM plan_mgmt.dba_plans;

-- Manual capture: the first plan of a statement is Approved.
SET pg_plan_mgmt.capture_plan_baselines = manual;
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
SELECT status, origin, enabled, valid, sql_text, plan_outline
  FROM plan_mgmt.dba_plans;

-- Planning the same statement again finds the existing plan.
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 2');
SELECT count(*) FROM plan_mgmt.dba_plans;

-- A different plan for the same statement is captured as Unapproved.
SET random_page_cost = 0.1;
SET cpu_index_tuple_cost = 0;
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
RESET random_page_cost;
RESET cpu_index_tuple_cost;
SELECT status, origin, plan_outline FROM plan_mgmt.dba_plans
 ORDER BY status, plan_outline;

-- Statements on catalogs or temporary tables are never captured.
SELECT count(*) > 0 FROM pg_class WHERE relname = 'pm_fact';
CREATE TEMP TABLE pm_temp (a int);
SELECT * FROM pm_temp;
SELECT count(*) FROM plan_mgmt.dba_plans;

-- Only the relevant statement of a multi-statement query string is stored.
SELECT 1 \; SELECT count(*) FROM pm_dim WHERE val = 3 \; SELECT 2;
SELECT sql_text FROM plan_mgmt.dba_plans WHERE sql_text LIKE '%val = 3%';

-- Automatic capture waits until a statement has been planned twice.
SELECT plan_mgmt.reset();
SET pg_plan_mgmt.capture_plan_baselines = automatic;
SELECT count(*) FROM pm_fact WHERE dim_id = 5;
SELECT count(*) FROM plan_mgmt.dba_plans;
SELECT count(*) FROM pm_fact WHERE dim_id = 6;
SELECT status, origin, sql_text FROM plan_mgmt.dba_plans;

-- Capture is privileged.
CREATE ROLE regress_pm_user;
SET ROLE regress_pm_user;
SET pg_plan_mgmt.capture_plan_baselines = manual;
SELECT * FROM plan_mgmt.dba_plans;
RESET ROLE;
DROP ROLE regress_pm_user;

SET pg_plan_mgmt.capture_plan_baselines = off;
SELECT plan_mgmt.reset();
