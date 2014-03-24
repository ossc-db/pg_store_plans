#! /usr/bin/perl

print <<'EOS';
\pset pager
SET client_min_messages = 'error';
CREATE EXTENSION IF NOT EXISTS pg_store_plans;
DROP TABLE IF EXISTS plans;
CREATE TABLE PLANS (id int, title text, plan text, inflated text);
SET client_min_messages = 'notice';
INSERT INTO PLANS (VALUES
EOS

$state = 0;
$first = 1;
while(<>) {
	chomp;
	if ($state == 0) {
		next if (!/^###### (Plan ([0-9]+):.*$)/);
		$title = $1; $plan_no = $2;
		if ($first) {
			$first = 0;
		} else {
			print ",\n\n";
		}
		print "-- ###### $title\n";
		$state = 1;
	} elsif ($state == 1) {
		if (/^######/) {
			die("??? : $_");
		}
		next if (!/^ *({[^ ].*})$/);
		$plan = $1;
		$escape = "";
		if ($plan =~ /'/ || $plan =~ /\\\"/) {
			$escape = "E";
		}

		# Add escape char for '''
		$plan =~ s/'/\\'/g;
		# Add escape char for '\"'
		$plan =~ s/\\\"/\\\\\"/g;

		print "($plan_no, \'$title\',\n";
		print " $escape'$plan')";
		$state = 0;
	}
}

print <<'EOS';
);
UPDATE plans SET inflated = pg_store_plans_jsonplan(plan);

\echo  ###### format conversion tests
SELECT '### '||'inflate-short    '||title||E'\n'||
  inflated
  FROM plans WHERE id BETWEEN 1 AND 3 ORDER BY id;
SELECT '### '||'yaml-short       '||title||E'\n'||
  pg_store_plans_yamlplan(plan)
  FROM plans WHERE id BETWEEN 4 AND 6 or id = 1 ORDER BY id;
SELECT '### '||'xml-short        '||title||E'\n'||
  pg_store_plans_xmlplan(plan)
  FROM plans WHERE id BETWEEN 7 AND 9 or id = 1 ORDER BY id;

\echo  ###### text format output test
SELECT '### '||'TEXT-short       '||title||E'\n'||
  pg_store_plans_textplan(plan)
  FROM plans ORDER BY id;

\echo  ###### long-json-as-a-source test
SELECT '### '||'inflate-long JSON'||title||E'\n'||
  pg_store_plans_jsonplan(inflated)
  FROM plans WHERE id BETWEEN 1 AND 3 ORDER BY id;
SELECT '### '||'yaml-long JSON   '||title||E'\n'||
  pg_store_plans_yamlplan(inflated)
  FROM plans WHERE id BETWEEN 4 AND 6 ORDER BY id;
SELECT '### '||'xml-long JSON    '||title||E'\n'||
  pg_store_plans_xmlplan(inflated)
  FROM plans WHERE id BETWEEN 7 AND 9 ORDER BY id;
SELECT '### '||'text-long JSON   '||title||E'\n'||
  pg_store_plans_xmlplan(inflated)
  FROM plans WHERE id BETWEEN 10 AND 12 ORDER BY id;

\echo  ###### chopped-source test
SELECT '### '||'inflate-chopped  '||title||E'\n'||
  pg_store_plans_jsonplan(substring(plan from 1 for char_length(plan) / 3))
  FROM plans WHERE id BETWEEN 13 AND 15 ORDER BY id;
SELECT '### '||'yaml-chopped     '||title||E'\n'||
  pg_store_plans_yamlplan(substring(plan from 1 for char_length(plan) / 3))
  FROM plans WHERE id BETWEEN 16 AND 18 ORDER BY id;
SELECT '### '||'xml-chopped      '||title||E'\n'||
  pg_store_plans_xmlplan(substring(plan from 1 for char_length(plan) / 3))
  FROM plans WHERE id BETWEEN 19 AND 21 ORDER BY id;
SELECT '### '||'text-chopped     '||title||E'\n'||
  pg_store_plans_textplan(substring(plan from 1 for char_length(plan) / 3))
  FROM plans WHERE id BETWEEN 22 AND 24 ORDER BY id;

\echo ###### shorten, normalize test
SELECT '### '||'shorten          '||title||E'\n'||
  pg_store_plans_shorten(inflated)
  FROM plans WHERE id BETWEEN 1 AND 3 ORDER BY id;
SELECT '### '||'normalize        '||title||E'\n'||
  pg_store_plans_shorten(inflated)
  FROM plans WHERE id BETWEEN 1 AND 3 ORDER BY id;

\echo ###### round-trip test
SELECT COUNT(*), SUM(success)
 FROM (SELECT CASE
	          WHEN pg_store_plans_shorten(inflated) = plan THEN 1 ELSE 0
              END as success
	   FROM plans) t;
EOS
