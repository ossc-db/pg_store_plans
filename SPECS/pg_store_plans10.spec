# SPEC file for pg_store_plans
# Copyright(C) 2020 NIPPON TELEGRAPH AND TELEPHONE CORPORATION

%define _pgdir   /usr/pgsql-10
%define _bindir  %{_pgdir}/bin
%define _libdir  %{_pgdir}/lib
%define _datadir %{_pgdir}/share
%if "%(echo ${MAKE_ROOT})" != ""
  %define _rpmdir %(echo ${MAKE_ROOT})/RPMS
  %define _sourcedir %(echo ${MAKE_ROOT})
%endif

## Set general information for pg_store_plans.
Summary:    Record executed plans on PostgreSQL 10
Name:       pg_store_plans10
Version:    1.3.1
Release:    1%{?dist}
License:    BSD
Group:      Applications/Databases
Source0:    %{name}-%{version}.tar.gz
#URL:        http://example.com/pg_store_plans/
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-%(%{__id_u} -n)
Vendor:     NIPPON TELEGRAPH AND TELEPHONE CORPORATION

## We use postgresql-devel package
BuildRequires:  postgresql10-devel
Requires:  postgresql10-libs

## Description for "pg_store_plans"
%description

pg_store_plans provides capability to record statistics for every plan
executed on PostgreSQL.

Note that this package is available for only PostgreSQL 10.

## pre work for build pg_store_plans
%prep
PATH=/usr/pgsql-10/bin:$PATH
if [ "${MAKE_ROOT}" != "" ]; then
  pushd ${MAKE_ROOT}
  make clean %{name}-%{version}.tar.gz
  popd
fi
if [ ! -d %{_rpmdir} ]; then mkdir -p %{_rpmdir}; fi
%setup -q

## Set variables for build environment
%build
PATH=/usr/pgsql-10/bin:$PATH
make USE_PGXS=1 %{?_smp_mflags}

## Set variables for install
%install
rm -rf %{buildroot}
install -d %{buildroot}%{_libdir}
install pg_store_plans.so %{buildroot}%{_libdir}/pg_store_plans.so
install -d %{buildroot}%{_datadir}/extension
install -m 644 pg_store_plans--1.2--1.3.sql %{buildroot}%{_datadir}/extension/pg_store_plans--1.2--1.3.sql
install -m 644 pg_store_plans--1.3--1.3.1.sql %{buildroot}%{_datadir}/extension/pg_store_plans--1.3--1.3.1.sql
install -m 644 pg_store_plans--1.3.1.sql %{buildroot}%{_datadir}/extension/pg_store_plans--1.3.1.sql
install -m 644 pg_store_plans.control %{buildroot}%{_datadir}/extension/pg_store_plans.control

%clean
rm -rf %{buildroot}

%files
%defattr(0755,root,root)
%{_libdir}/pg_store_plans.so
%defattr(0644,root,root)
%{_datadir}/extension/pg_store_plans--1.2--1.3.sql
%{_datadir}/extension/pg_store_plans--1.3--1.3.1.sql
%{_datadir}/extension/pg_store_plans--1.3.1.sql
%{_datadir}/extension/pg_store_plans.control

# History of pg_store_plans.
%changelog
* Tue Jan 30 2020 Kyotaro Horiguchi
- Fixed a bug.
* Tue Jan 22 2019 Kyotaro Horiguchi
- Fixed a bug.
* Tue Oct 10 2017 Kyotaro Horiguchi
- Supports PostgreSQL 10
* Fri Aug 26 2016 Kyotaro Horiguchi
- Some fix in plan representation functions.
* Wed Apr 13 2016 Kyotaro Horiguchi
- Support PostgreSQL 9.5
* Fri Jun 12 2015 Kyotaro Horiguchi
- Initial version.


