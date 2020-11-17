\set format json
\pset pager

set work_mem = '1MB';
drop table if exists tt1;
drop table if exists tt2;
drop table if exists tt3;
drop table if exists p cascade;
drop table if exists ct1;
create table p (a int, b int, c text);
create table tt1 (a int, b int not null, c text) inherits (p);
create table tt2 (a int, b int, c text) inherits (p);
create table tt3 (a int, b int, c text) inherits (p);
create index i_tt1 on tt1(a);
create index i_tt2 on tt2(a);
create index i_tt3_a on tt3(a);
create index i_tt3_b on tt3(b);
create table ct1 (a int unique, b int);
insert into ct1 values (1,1), (2,2);

create or replace function t_tt1_1() returns trigger as $$
  BEGIN
    NEW.b := -NEW.a;
    RETURN NEW;
  END;
$$ language plpgsql;
create or replace function t_tt1_2() returns trigger as $$
  BEGIN
    NEW.c := 'tt1';
    RETURN NEW;
  END;
$$ language plpgsql;
create trigger tt1_trig_1 before insert or update on tt1
  for each row execute procedure t_tt1_1();
create trigger tt1_trig_2 before insert or update on tt1
  for each row execute procedure t_tt1_2();
insert into tt2 (select a, -a, 'tt2' from generate_series(7000, 17000) a);
insert into tt3 (select a, -a, 'tt3' from generate_series(0, 100000) a);
insert into tt3 (select 5000,  a, 'tt3' from generate_series(0, 40000) a);
insert into tt3 (select a,  555, 'tt3' from generate_series(0, 40000) a);

\echo ###### Insert, Trigger
explain (analyze on, buffers on, verbose on, format :format)
   insert into tt1 (select a from generate_series(0, 10000) a);

\echo ###### Update, Trigger
explain (analyze on, buffers on, verbose on, format :format)
   update tt1 set a = a + 1;
\echo ###### Delete
explain (analyze on, buffers on, verbose on, format :format)
   delete from tt1 where a % 10 = 0;

----
delete from tt1;
insert into tt1 (select a from generate_series(0, 10000) a);
analyze;

\echo ###### Result, Append Seq Scan
explain (analyze on, buffers on, verbose on, format :format)
   select *, 1 from
   (select a + 1, 3 from tt1 union all select a, 4 from tt2) as x;
\echo ###### Index scan (forward) ANY, array in expr, escape
explain (analyze on, buffers on, verbose on, format :format)
   select * from tt1 "x""y" where a in (50, 120, 300, 500);
\echo ###### Index scan (backward), MergeJoin, Sort, quicksort, alias
explain (analyze on, buffers on, verbose on, format :format)
   select x.b, x.c  from tt1 x join tt2 y on (x.a = -y.b * 3)
   order by x.a desc limit 10;
\echo ###### IndexOnlyScan
explain (analyze on, buffers on, verbose on, format :format)
    select a from tt1 where a < 10;
\echo ###### Plain Aggregate, CTE, Recursive Union, WorkTable Scan, CTE Scan
explain (analyze on, buffers on, verbose on, format :format)
   with recursive cte1(a) as
      (select 1 union all
       select a + 1 from cte1 where a < 10)
   select sum(a) from cte1;
\echo ###### FunctionScan, Hash/HashJoin, Nested Loop
explain (analyze on, buffers on, verbose on, format :format)
   select datname from pg_stat_activity;
\echo ###### MergeAppend, Values
explain (analyze on, buffers on, verbose on, format :format)
   (select a from tt1 order by a) union all
   (select a from (values (100), (200), (300)) as tv(a))
    order by a;
\echo ###### Append, HashAggregate
explain (analyze on, buffers on, verbose on, format :format)
   select a from tt1 union select b from tt2;
\echo ###### GroupAggregate
set work_mem = '128kB';
explain (analyze on, buffers on, verbose on, format :format)
   select sum(a) from tt1 group by b;
set work_mem = '1MB';
\echo ###### Group
set work_mem = '128kB';
explain (analyze on, buffers on, verbose on, format :format)
   select b from tt1 group by b;
set work_mem = '1MB';
\echo ###### SetOp intersect, SubqueryScan
explain (analyze on, buffers on, verbose on, format :format)
   select a from tt1 intersect select b from tt2 order by a;
\echo ###### Sorted SetOp, Sort on Disk
set work_mem = '128kB';
explain (analyze on, buffers on, verbose on, format :format)
   select a from tt1 intersect select b from tt2 order by a;
set work_mem = '1MB';
\echo ###### HashSetOp intersect All, SubqueryScan
explain (analyze on, buffers on, verbose on, format :format)
   select a from tt1 intersect all select b from tt2 order by a;
\echo ###### HashSetOp except, SubqueryScan
explain (analyze on, buffers on, verbose on, format :format)
   select a from tt1 except select b from tt2 order by a;
\echo ###### HashSetOp except all, SubqueryScan
explain (analyze on, buffers on, verbose on, format :format)
   select a from tt1 except all select b from tt2 order by a;
\echo ###### merge LEFT join
set work_mem = '64kB';
explain (analyze on, buffers on, verbose on, format :format)
   select x.b from tt1 x left join tt3 y on (x.a = y.a);
set work_mem = '1MB';
\echo ###### hash FULL join
explain (analyze on, buffers on, verbose on, format :format)
   select x.b from tt1 x full outer join tt2 y on (x.a = y.a);
\echo ###### hash SEMI join
explain (analyze on, buffers on, verbose on, format :format)
   select * from tt1 where a = any(select b from tt2);
\echo ###### Hash Anti Join
explain (analyze on, buffers on, verbose on, format :format)
   select * from tt1 where not exists (select * from tt2 where tt1.a = tt2.b);
\echo ###### WindowAgg
explain (analyze on, buffers on, verbose on, format :format)
   select first_value(a) over (partition by a / 10) from tt1;
\echo ###### Unique
explain (analyze on, buffers on, verbose on, format :format)
   select distinct a from tt1 order by a;
\echo ###### PlainAggregate
explain (analyze on, buffers on, verbose on, format :format)
   select sum(a) from tt1;
\echo ###### BitmapIndexScan/BitmapHeapScan, BitmapOr, lossy
set enable_seqscan to false;
set work_mem to '64kB';
explain (analyze on, buffers on, verbose on, format :format)
   select * from tt3 where b > -99998;
\echo ###### Join Filter
set enable_seqscan to true;
set enable_indexscan to false;
set enable_bitmapscan to false;
explain (analyze on, buffers on, verbose on, format :format)
   SELECT tt2.* from tt2
   LEFT OUTER JOIN tt3 ON (tt2.a < tt3.a) where tt3.a + tt2.a < 100000
   LIMIT 100;
reset enable_seqscan;
reset enable_indexscan;
reset enable_bitmapscan;
reset work_mem;
\echo ###### TidScan
explain (analyze on, buffers on, verbose on, format :format)
   select * from tt3 where ctid = '(0,28)';
\echo ###### LockRows
begin;
explain (analyze on, buffers on, verbose on, format :format)
   select a from tt1 where a % 10 = 0 for update;
rollback;
\echo ###### Materialize
explain (analyze on, buffers on, verbose on, format :format)
   select * from tt1 where a = all(select b from tt2);
\echo ###### Update on partitioned tables
explain (analyze on, buffers on, verbose on, format :format)
   UPDATE p SET b = b + 1;
\echo ###### Delete on partitioned tables
explain (analyze on, buffers on, verbose on, format :format)
   DELETE FROM p WHERE a = 100;
\echo ###### ON CONFLICT
explain (analyze on, buffers on, verbose on, format :format)
   INSERT INTO ct1 VALUES (1,1) ON CONFLICT (a) DO UPDATE SET b = EXCLUDED.b + 1;
\echo ###### GROUP BY
explain (analyze on, buffers on, verbose on, format :format)
   SELECT a, b, max(c) FROM tt1 GROUP BY a, b;
\echo ###### GROUPING SETS
explain (analyze on, buffers on, verbose on, format :format)
   SELECT a, b, max(c) FROM tt1 GROUP BY GROUPING SETS ((a), (b), ());
\echo ###### Table sample
explain (analyze on, buffers on, verbose on, format :format)
   SELECT * FROM tt1 TABLESAMPLE system(1) REPEATABLE (1);

\echo ###### Project Set
explain (analyze on, buffers on, verbose on, format :format)
   SELECT * from XMLTABLE('//towns/town'
    PASSING BY REF '<towns><town><name>Toronto</name></town><town><name>Ottawa</name></town></towns>'
	 COLUMNS name text);

\echo ###### Incremental Sort
explain (analyze on, buffers on, verbose on, format :format)
	WITH x AS (SELECT i/100 + 1 AS a, i + 1 AS b FROM generate_series(0, 999) i)
	  SELECT * FROM (SELECT * FROM x ORDER BY a) s ORDER BY a, b LIMIT 31;

-- Named Tuplestore Scan -- requires auto_explain
DROP TABLE IF EXISTS e1 CASCADE;
CREATE TABLE e1 (a int, b int);
CREATE OR REPLACE function e1_t1() RETURNS TRIGGER AS $$
DECLARE
  total int;
BEGIN
  SELECT sum(a) INTO total FROM post;
  NEW.b := total;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;
CREATE TRIGGER e1_t1 AFTER INSERT ON e1
 REFERENCING NEW TABLE AS post
 FOR EACH ROW EXECUTE PROCEDURE e1_t1();

INSERT INTO e1 VALUES (1, 1);

load 'auto_explain';
set auto_explain.log_min_duration to 0;
set auto_explain.log_analyze to true;
set auto_explain.log_buffers to true;
set auto_explain.log_buffers to true;
set auto_explain.log_format to :format;
set auto_explain.log_timing  to true;
set auto_explain.log_nested_statements to true;
set client_min_messages to LOG;
set log_min_messages to FATAL; -- Inhibit LOG by auto_explain
\echo ###### Named Tuplestore Scan
CREATE TRIGGER e1_t2 AFTER UPDATE ON e1
 REFERENCING NEW TABLE AS post OLD TABLE AS pre
 FOR EACH ROW EXECUTE PROCEDURE e1_t1();
UPDATE e1 SET a = a + 1;
set client_min_messages to DEFAULT;
set log_min_messages to DEFAULT;
set auto_explain.log_min_duration to -1;

-- ###### Parallel
drop table if exists lt1;
create table lt1 (a int, b text);
alter table lt1 alter column b set storage plain;
insert into lt1 (select a, repeat('x', 1000) from generate_series(0, 99999) a);
set max_parallel_workers_per_gather to 2;
set parallel_tuple_cost to 0;
set parallel_setup_cost to 0;
set min_parallel_table_scan_size to 0;
set min_parallel_index_scan_size to 0;

\echo ###### Parallel Seq Scan
explain (analyze on, buffers on, verbose on, format :format)
   SELECT * FROM lt1;

\echo ###### Parallel Index Scan
explain (analyze on, buffers on, verbose on, format :format)
   SELECT * FROM tt1 where a < 100;

\echo ###### Gather Merge
explain (analyze on, buffers on, verbose on, format :format)
   SELECT a FROM tt1 ORDER BY a;

-- BitmapAnd/Inner/Right/ForegnScan
