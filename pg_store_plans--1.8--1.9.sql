/*
 * pg_store_plans/pg_store_plans--1.8--1.9.sql
 *
 * 업그레이드 스크립트: pg_store_plans 1.8 -> 1.9
 *
 * - 기존 함수 및 뷰 삭제
 * - PostgreSQL 17 이상/미만 버전별 함수(분기 처리) 재생성
 * - 뷰 생성 및 권한 부여
 */

-- psql에서 직접 실행되는 것을 방지 (CREATE EXTENSION을 통해서만 실행)
\echo Use "CREATE EXTENSION pg_store_plans" to load this file. \quit

-- 기존 뷰 및 함수 삭제 (업그레이드 전 정리)
DROP VIEW pg_store_plans;
DROP FUNCTION pg_store_plans();

-- PostgreSQL 버전에 따라 메인 함수 생성 (17 이상/미만 분기)
DO
$$
BEGIN
    IF (SELECT split_part(setting,'.',1) FROM pg_settings WHERE name = 'server_version')::int >= 17 THEN
        -- PostgreSQL 17 이상: shared/local 블록 시간 컬럼 분리
        CREATE FUNCTION pg_store_plans(
            OUT userid oid,
            OUT dbid oid,
            OUT queryid int8,
            OUT planid int8,
            OUT plan text,
            OUT calls int8,
            OUT total_time float8,
            OUT min_time float8,
            OUT max_time float8,
            OUT mean_time float8,
            OUT stddev_time float8,
            OUT rows int8,
            OUT shared_blks_hit int8,
            OUT shared_blks_read int8,
            OUT shared_blks_dirtied int8,
            OUT shared_blks_written int8,
            OUT local_blks_hit int8,
            OUT local_blks_read int8,
            OUT local_blks_dirtied int8,
            OUT local_blks_written int8,
            OUT temp_blks_read int8,
            OUT temp_blks_written int8,
            OUT shared_blk_read_time float8,
            OUT shared_blk_write_time float8,
            OUT local_blk_read_time float8,
            OUT local_blk_write_time float8,
            OUT temp_blk_read_time float8,
            OUT temp_blk_write_time float8,
            OUT first_call timestamptz,
            OUT last_call timestamptz
        )
        RETURNS SETOF record
        AS 'MODULE_PATHNAME', 'pg_store_plans_1_9'
        LANGUAGE C
        VOLATILE PARALLEL SAFE;
    ELSE
        -- PostgreSQL 17 미만: blk_read_time, blk_write_time 사용
        CREATE FUNCTION pg_store_plans(
            OUT userid oid,
            OUT dbid oid,
            OUT queryid int8,
            OUT planid int8,
            OUT plan text,
            OUT calls int8,
            OUT total_time float8,
            OUT min_time float8,
            OUT max_time float8,
            OUT mean_time float8,
            OUT stddev_time float8,
            OUT rows int8,
            OUT shared_blks_hit int8,
            OUT shared_blks_read int8,
            OUT shared_blks_dirtied int8,
            OUT shared_blks_written int8,
            OUT local_blks_hit int8,
            OUT local_blks_read int8,
            OUT local_blks_dirtied int8,
            OUT local_blks_written int8,
            OUT temp_blks_read int8,
            OUT temp_blks_written int8,
            OUT blk_read_time float8,
            OUT blk_write_time float8,
            OUT temp_blk_read_time float8,
            OUT temp_blk_write_time float8,
            OUT first_call timestamptz,
            OUT last_call timestamptz
        )
        RETURNS SETOF record
        AS 'MODULE_PATHNAME', 'pg_store_plans_1_7'
        LANGUAGE C
        VOLATILE PARALLEL SAFE;
    END IF;
END
$$ LANGUAGE plpgsql;

-- 메인 함수에 대한 뷰 생성 (사용 편의성 제공)
CREATE VIEW pg_store_plans AS
  SELECT * FROM pg_store_plans();

-- 모든 사용자에게 SELECT 권한 부여
GRANT SELECT ON pg_store_plans TO PUBLIC;
