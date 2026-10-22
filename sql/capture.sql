CREATE EXTENSION pg_plan_mgmt;

-- EXPLAIN without costs.
CREATE FUNCTION explain_masked(query text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || query
	LOOP
		RETURN NEXT ln;
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
SELECT count(*) FROM plan_mgmt.plans;

-- Manual capture: the first plan of a statement is Approved.
SET pg_plan_mgmt.capture_plan_baselines = manual;
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
SELECT status, origin, enabled, valid, sql_text, plan_outline
  FROM plan_mgmt.plans;

-- Planning the same statement again finds the existing plan.
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 2');
SELECT count(*) FROM plan_mgmt.plans;

-- A different plan for the same statement is captured as Unapproved.
SET random_page_cost = 0.1;
SET cpu_index_tuple_cost = 0;
SELECT explain_masked('SELECT * FROM pm_fact f JOIN pm_dim d ON f.dim_id = d.id WHERE d.val = 1');
RESET random_page_cost;
RESET cpu_index_tuple_cost;
SELECT status, origin, plan_outline FROM plan_mgmt.plans
 ORDER BY status, plan_outline;

-- Statements on catalogs or temporary tables are never captured.
SELECT count(*) > 0 FROM pg_class WHERE relname = 'pm_fact';
CREATE TEMP TABLE pm_temp (a int);
SELECT * FROM pm_temp;
SELECT count(*) FROM plan_mgmt.plans;

-- Only the relevant statement of a multi-statement query string is stored.
SELECT 1 \; SELECT count(*) FROM pm_dim WHERE val = 3 \; SELECT 2;
SELECT sql_text FROM plan_mgmt.plans WHERE sql_text LIKE '%val = 3%';

-- Automatic capture waits until a statement has been planned twice.
SELECT plan_mgmt.reset();
SET pg_plan_mgmt.capture_plan_baselines = automatic;
SELECT count(*) FROM pm_fact WHERE dim_id = 5;
SELECT count(*) FROM plan_mgmt.plans;
SELECT count(*) FROM pm_fact WHERE dim_id = 6;
SELECT status, origin, sql_text FROM plan_mgmt.plans;

-- Capture is privileged.
CREATE ROLE regress_pm_user LOGIN;
SET ROLE regress_pm_user;
SET pg_plan_mgmt.capture_plan_baselines = manual;
SELECT * FROM plan_mgmt.plans;
RESET ROLE;

-- Even with automatic capture configured for the database, nothing a
-- non-superuser session plans is captured, and the text of statements
-- captured by others is hidden from it.
GRANT USAGE ON SCHEMA plan_mgmt TO regress_pm_user;
GRANT SELECT ON plan_mgmt.plans TO regress_pm_user;
GRANT EXECUTE ON FUNCTION plan_mgmt.get_plans() TO regress_pm_user;
GRANT SELECT ON pm_fact TO regress_pm_user;
SELECT current_database() AS db, current_user AS su \gset
ALTER DATABASE :"db" SET pg_plan_mgmt.capture_plan_baselines = automatic;
\c - regress_pm_user
SHOW pg_plan_mgmt.capture_plan_baselines;
SELECT count(*) FROM pm_fact WHERE dim_id = 7;
SELECT count(*) FROM pm_fact WHERE dim_id = 7;
SELECT sql_text FROM plan_mgmt.plans;
\c - :su
ALTER DATABASE :"db" RESET pg_plan_mgmt.capture_plan_baselines;
SELECT sql_text FROM plan_mgmt.plans;
DROP OWNED BY regress_pm_user;
DROP ROLE regress_pm_user;

SET pg_plan_mgmt.capture_plan_baselines = off;
SELECT plan_mgmt.reset();
