SET client_min_messages = 'error';
CREATE EXTENSION IF NOT EXISTS pg_store_plans;
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;
SELECT pg_stat_statements_reset();
SELECT pg_store_plans_reset();

DROP TABLE IF EXISTS t1;
CREATE TABLE t1 (a int);
CREATE INDEX ON t1 (a);
INSERT INTO t1 (SELECT a FROM generate_series(0, 9999) a);
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*) FROM (SELECT * FROM t1) AS x;
SET enable_seqscan TO false;
SELECT count(*) FROM (SELECT * FROM t1) AS x;
SELECT count(*) FROM (SELECT * FROM t1) AS x;
SET enable_bitmapscan TO false;
SELECT count(*) FROM (SELECT * FROM t1) AS x;
SELECT count(*) FROM (SELECT * FROM t1) AS x;
SELECT count(*) FROM (SELECT * FROM t1) AS x;
RESET enable_seqscan;
RESET enable_bitmapscan;

CREATE OR REPLACE FUNCTION test_explain() RETURNS text AS
$x$
DECLARE
    r record;
    s text;
    p text;
    totalrows int;
    totalcalls int;
    first bool;
BEGIN
    s := '';
    first = true;
    SELECT calls, rows INTO totalcalls, totalrows
    FROM pg_stat_statements
    WHERE query = 'SELECT count(*) FROM (SELECT * FROM t1) AS x';

    FOR r IN SELECT s.query as q, p.plan as p, p.calls as c, p.rows r
             FROM pg_stat_statements s
             JOIN pg_store_plans p USING (queryid)
             WHERE s.query = 'SELECT count(*) FROM (SELECT * FROM t1) AS x'
             ORDER BY p.calls
    LOOP
      IF first then
        s = r.q || E'\n  totalcalls=' || totalcalls ||
            ' , totalrows=' || totalrows || E'\n';
        first := false;
      END IF;
      p := regexp_replace(r.p, '=[0-9.]+([^0-9.])', '=xxx\1', 'g');
      s := s || p || E'\n  calls=' || r.c || ', rows=' || r.r || E'\n';
    END LOOP;

    RETURN s;
END
$x$
LANGUAGE plpgsql;
SELECT test_explain();
DROP FUNCTION test_explain();
DROP TABLE t1;

