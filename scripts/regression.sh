#!/usr/bin/env bash

set -ex

export DEBIAN_FRONTEND=noninteractive
apt update
apt install -y curl ca-certificates gnupg lsb-release build-essential
curl https://www.postgresql.org/media/keys/ACCC4CF8.asc | apt-key add -
echo "deb http://apt.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" > /etc/apt/sources.list.d/pgdg.list
apt update

export PGUSER=postgres
export PGDATABASE=postgres
export PGPORT=5432

apt install -y postgresql-13 postgresql-server-dev-13
echo 'local all all trust' > /etc/postgresql/13/main/pg_hba.conf
/etc/init.d/postgresql start
export PG_CONFIG=/usr/lib/postgresql/13/bin/pg_config
make clean && make && make install
psql -Atc 'alter system set shared_preload_libraries to pg_store_plans, pg_stat_statements'
/etc/init.d/postgresql restart
make installcheck
