It is a [fork](http://github.com/ossc-db/pg_store_plans) with changes:

* Added GUC store_last_plan (false by default)
  last plan is not saved, only first one, gets +30% increase.
* Does not save queryID in its own format, gives up to +10% increase.
* Added sample_rate (based on random).
* Added slow_statement_duration - this is an unconditional logging of query plans longer than 
  the specified value.
* GUC min_duration - now be specified as time.
* GUC pg_store_plans.track_planning - track planning time.
* make fixes.
