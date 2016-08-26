# SPEC file for pg_store_plans
# Copyright(C) 2015 NIPPON TELEGRAPH AND TELEPHONE CORPORATION

%define _pgdir   /usr/pgsql-9.4
%define _bindir  %{_pgdir}/bin
%define _libdir  %{_pgdir}/lib
%define _datadir %{_pgdir}/share
%if "%(echo ${MAKE_ROOT})" != ""
  %define _rpmdir %(echo ${MAKE_ROOT})/RPMS
  %define _sourcedir %(echo ${MAKE_ROOT})
%endif

## Set general information for pg_store_plans.
Summary:    Record executed plans on PostgreSQL 9.4
Name:       pg_store_plans94
Version:    1.1
Release:    1%{?dist}
License:    BSD
Group:      Applications/Databases
Source0:    %{name}-%{version}.tar.gz
#URL:        http://example.com/pg_store_plans/
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-%(%{__id_u} -n)
Vendor:     NIPPON TELEGRAPH AND TELEPHONE CORPORATION

## We use postgresql-devel package
BuildRequires:  postgresql94-devel
Requires:  postgresql94-libs

## Description for "pg_store_plans"
%description

pg_store_plans provides capability to record statistics for every plan
executed on PostgreSQL.

Note that this package is available for only PostgreSQL 9.4.

## pre work for build pg_store_plans
%prep
PATH=/usr/pgsql-9.4/bin:$PATH
if [ "${MAKE_ROOT}" != "" ]; then
  pushd ${MAKE_ROOT}
  make clean %{name}-%{version}.tar.gz
  popd
fi
if [ ! -d %{_rpmdir} ]; then mkdir -p %{_rpmdir}; fi
%setup -q

## Set variables for build environment
%build
PATH=/usr/pgsql-9.4/bin:$PATH
make USE_PGXS=1 %{?_smp_mflags}

## Set variables for install
%install
rm -rf %{buildroot}
install -d %{buildroot}%{_libdir}
install pg_store_plans.so %{buildroot}%{_libdir}/pg_store_plans.so
install -d %{buildroot}%{_datadir}/extension
install -m 644 pg_store_plans--1.1.sql %{buildroot}%{_datadir}/extension/pg_store_plans--1.1.sql
install -m 644 pg_store_plans--1.0--1.1.sql %{buildroot}%{_datadir}/extension/pg_store_plans--1.0--1.1.sql
install -m 644 pg_store_plans.control %{buildroot}%{_datadir}/extension/pg_store_plans.control

%clean
rm -rf %{buildroot}

%files
%defattr(0755,root,root)
%{_libdir}/pg_store_plans.so
%defattr(0644,root,root)
%{_datadir}/extension/pg_store_plans--1.1.sql
%{_datadir}/extension/pg_store_plans--1.0--1.1.sql
%{_datadir}/extension/pg_store_plans.control

# History of pg_store_plans.
%changelog
* Fri Aug 26 2016 Kyotaro Horiguchi
- Some fixes in text representaion.
* Fri Jun 12 2015 Kyotaro Horiguchi
- Initial version.


