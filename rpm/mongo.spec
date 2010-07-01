Name: mongo
Version: 1.5.3
Release: mongodb_1%{?dist}
Summary: mongo client shell and tools
License: AGPL 3.0
URL: http://www.mongodb.org
Group: Applications/Databases

Source0: http://downloads.mongodb.org/src/mongodb-src-r%{version}.tar.gz
Source1: mongod.conf
Source2: init.d-mongod
Source3: mongod.sysconfig
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
BuildRequires: js-devel, readline-devel, boost-devel, pcre-devel, scons
BuildRequires: gcc-c++, scons, libstdc++-devel, ncurses-devel

%description
Mongo (from "huMONGOus") is a schema-free document-oriented database.
It features dynamic profileable queries, full indexing, replication
and fail-over support, efficient storage of large binary data objects,
and auto-sharding.

This package provides the mongo shell, import/export tools, and other
client utilities.

%package server
Summary: mongo server, sharding server, and support scripts
Group: Applications/Databases
Requires: mongo

%description server
Mongo (from "huMONGOus") is a schema-free document-oriented database.

This package provides the mongo server software, mongo sharding server
softwware, default configuration files, and init.d scripts.

%package devel
Summary: Headers and libraries for mongo development. 
Group: Applications/Databases

%description devel
Mongo (from "huMONGOus") is a schema-free document-oriented database.

This package provides the mongo static library and header files needed
to develop mongo client software.

%prep
%setup -q -n mongodb-src-r%{version}

%build
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

scons all %{?_smp_mflags}
# XXX really should have shared library here

%install
# %{?_smp_mflags} should work for scons as well
scons --prefix=%{buildroot}/%{_prefix} install %{?_smp_mflags}

%{__mkdir_p} %{buildroot}/%{_datadir}/man/man1
%{__mkdir_p} %{buildroot}/%{_initrddir}
%{__mkdir_p} %{buildroot}/%{_sysconfdir}/sysconfig
%{__mkdir_p} %{buildroot}/%{_var}/lib/mongo
%{__mkdir_p} %{buildroot}/%{_var}/log/mongo

%{__cp} %{_builddir}/mongodb-src-r%{version}/debian/*.1 %{buildroot}/%{_datadir}/man/man1/

%{__cp} %{_sourcedir}/init.d-mongod %{buildroot}/%{_initrddir}/mongod
%{__cp} %{_sourcedir}/mongod.conf %{buildroot}/%{_sysconfdir}/mongod.conf
%{__cp} %{_sourcedir}/mongod.sysconfig %{buildroot}/%{_sysconfdir}/sysconfig/mongod

%clean
scons -c
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

%pre server
if ! /usr/bin/id -g mongod &>/dev/null; then
    /usr/sbin/groupadd -r mongod
fi
if ! /usr/bin/id mongod &>/dev/null; then
    /usr/sbin/useradd -M -r -g mongod -d /var/lib/mongo -s /bin/false \
	-c mongod mongod > /dev/null 2>&1
fi

%post server
if test $1 = 1
then
  /sbin/chkconfig mongod on
fi

%preun server
if test $1 = 0
then
  /sbin/chkconfig --del mongod
fi

%postun server
if test $1 -ge 1
then
  /sbin/service mongod stop >/dev/null 2>&1 || :
fi

%files
%defattr(-,root,root,-)
%doc README GNU-AGPL-3.0.txt

%attr(0755,root,root) %{_bindir}/mongo
%attr(0755,root,root) %{_bindir}/mongodump
%attr(0755,root,root) %{_bindir}/mongoexport
%attr(0755,root,root) %{_bindir}/mongofiles
%attr(0755,root,root) %{_bindir}/mongoimport
%attr(0755,root,root) %{_bindir}/mongorestore
%attr(0755,root,root) %{_bindir}/mongostat

%{_mandir}/man1/mongo.1*
%{_mandir}/man1/mongod.1*
%{_mandir}/man1/mongodump.1*
%{_mandir}/man1/mongoexport.1*
%{_mandir}/man1/mongofiles.1*
%{_mandir}/man1/mongoimport.1*
%{_mandir}/man1/mongosniff.1*
%{_mandir}/man1/mongostat.1*
%{_mandir}/man1/mongorestore.1*

%files server
%defattr(-,root,root,-)

%config(noreplace) %{_sysconfdir}/mongod.conf

%attr(0755,root,root) %{_bindir}/mongod
%attr(0755,root,root) %{_bindir}/mongos
%attr(0755,root,root) %{_sysconfdir}/rc.d/init.d/mongod

%{_mandir}/man1/mongos.1*
%{_sysconfdir}/sysconfig/mongod

%attr(0750,mongod,mongod) %dir %{_var}/lib/mongo
%attr(0750,mongod,mongod) %dir %{_var}/log/mongo

%files devel
#%{_includedir}/mongo
%{_libdir}/libmongoclient.a
#%{_libdir}/libmongotestfiles.a

%changelog
* Thu Jul 1 2010 Mikko Koppanen <mikko@ibuildings.com>
- Use _smp_mflags for scons
- Reverted the config file location back to /etc/mongod.conf

* Tue Jun 29 2010 Mikko Koppanen <mikko@ibuildings.com>
- Use macros for commands and directories
- Add missing source files
- Added explicit attributes for binaries
- Changed default config location to /etc/mongo/mongod.conf

* Thu Jan 28 2010 Richard M Kreuter <richard@10gen.com>
- Minor fixes.

* Sat Oct 24 2009 Joe Miklojcik <jmiklojcik@shopwiki.com> - 
- Wrote mongo.spec.
