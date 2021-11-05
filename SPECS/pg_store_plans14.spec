# SPEC file for pg_store_plans
# Copyright(c) 2021, NIPPON TELEGRAPH AND TELEPHONE CORPORATION

%define _pgdir   /usr/pgsql-14
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
Summary:    Record executed plans on PostgreSQL 14
Name:       pg_store_plans14
Version:    1.6
Release:    1%{?dist}
License:    BSD
Group:      Applications/Databases
Source0:    %{name}-%{version}.tar.gz
#URL:        http://example.com/pg_store_plans/
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-%(%{__id_u} -n)
Vendor:     NIPPON TELEGRAPH AND TELEPHONE CORPORATION

## We use postgresql-devel package
BuildRequires:  postgresql14-devel
Requires:  postgresql14-libs

## Description for "pg_store_plans"
%description

pg_store_plans provides capability to record statistics for every plan
executed on PostgreSQL.

Note that this package is available for only PostgreSQL 14.

%package llvmjit
Requires: postgresql14-server, postgresql14-llvmjit
Requires: pg_store_plans14 = 1.6
Summary:  Just-in-time compilation support for pg_store_plans14

%description llvmjit
Just-in-time compilation support for pg_store_plans14

## pre work for build pg_store_plans
%prep
PATH=/usr/pgsql-14/bin:$PATH
if [ "${MAKE_ROOT}" != "" ]; then
  pushd ${MAKE_ROOT}
  make clean %{name}-%{version}.tar.gz
  popd
fi
if [ ! -d %{_rpmdir} ]; then mkdir -p %{_rpmdir}; fi
%setup -q

## Set variables for build environment
%build
PATH=/usr/pgsql-14/bin:$PATH
pg_config
make USE_PGXS=1 %{?_smp_mflags}

## Set variables for install
%install
rm -rf %{buildroot}
PATH=/usr/pgsql-14/bin:$PATH
make install DESTDIR=%{buildroot}

%clean
rm -rf %{buildroot}

%files
%defattr(0755,root,root)
%{_libdir}/pg_store_plans.so
%defattr(0644,root,root)
%{_datadir}/extension/pg_store_plans--1.6.sql
%{_datadir}/extension/pg_store_plans.control

%files llvmjit
%defattr(0644,root,root)
%{_bcdir}/pg_store_plans.index.bc
%{_mybcdir}/*.bc

# History of pg_store_plans.
%changelog
* Wed Nov xx 2021 Tatsuro Yamada, Julien Rouhaud, Kyotaro Horiguchi
- Version 1.6. Supports PostgreSQL 14
* Wed Jan 27 2021 Kyotaro Horiguchi
- Version 1.5. Supports PostgreSQL 13
* Thu Jan 30 2020 Kyotaro Horiguchi
- Version 1.4. Supports PostgreSQL 12
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


