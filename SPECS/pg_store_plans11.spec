# SPEC file for pg_store_plans
# Copyright(C) 2019 NIPPON TELEGRAPH AND TELEPHONE CORPORATION

%define _pgdir   /usr/pgsql-11
%define _bindir  %{_pgdir}/bin
%define _libdir  %{_pgdir}/lib
%define _datadir %{_pgdir}/share
%define _bcdir %{_libdir}/bitcode
%define _mybcdir %{_bcdir}/pg_store_plans

%if "%(echo ${MAKE_ROOT})" != ""
  %define _rpmdir %(echo ${MAKE_ROOT})/RPMS
  %define _sourcedir %(echo ${MAKE_ROOT})
%endif

## Set general information for pg_store_plans.
Summary:    Record executed plans on PostgreSQL 11
Name:       pg_store_plans11
Version:    1.3
Release:    2%{?dist}
License:    BSD
Group:      Applications/Databases
Source0:    %{name}-%{version}.tar.gz
#URL:        http://example.com/pg_store_plans/
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-%(%{__id_u} -n)
Vendor:     NIPPON TELEGRAPH AND TELEPHONE CORPORATION

## We use postgresql-devel package
BuildRequires:  postgresql11-devel
Requires:  postgresql11-libs

## Description for "pg_store_plans"
%description

pg_store_plans provides capability to record statistics for every plan
executed on PostgreSQL.

Note that this package is available for only PostgreSQL 11.

%package llvmjit
Requires: postgresql11-server, postgresql11-llvmjit
Requires: pg_store_plans11 = 1.3
Summary:  Just-in-time compilation support for pg_store_plans11

%description llvmjit
Just-in-time compilation support for pg_store_plans11

## pre work for build pg_store_plans
%prep
PATH=/usr/pgsql-11/bin:$PATH
if [ "${MAKE_ROOT}" != "" ]; then
  pushd ${MAKE_ROOT}
  make clean %{name}-%{version}.tar.gz
  popd
fi
if [ ! -d %{_rpmdir} ]; then mkdir -p %{_rpmdir}; fi
%setup -q

## Set variables for build environment
%build
PATH=/usr/pgsql-11/bin:$PATH
pg_config
make USE_PGXS=1 %{?_smp_mflags}

## Set variables for install
%install
rm -rf %{buildroot}
PATH=/usr/pgsql-11/bin:$PATH
make install DESTDIR=%{buildroot}

%clean
rm -rf %{buildroot}

%files
%defattr(0755,root,root)
%{_libdir}/pg_store_plans.so
%defattr(0644,root,root)
%{_datadir}/extension/pg_store_plans--1.3.sql
%{_datadir}/extension/pg_store_plans.control

%files llvmjit
%defattr(0644,root,root)
%{_bcdir}/pg_store_plans.index.bc
%{_mybcdir}/*.bc

# History of pg_store_plans.
%changelog
* Tue Jan 22 2019 Kyotaro Horiguchi
- Supports PostgreSQL 11
* Tue Oct 10 2017 Kyotaro Horiguchi
- Supports PostgreSQL 10
* Fri Aug 26 2016 Kyotaro Horiguchi
- Some fix in plan representation functions.
* Wed Apr 13 2016 Kyotaro Horiguchi
- Support PostgreSQL 9.5
* Fri Jun 12 2015 Kyotaro Horiguchi
- Initial version.


