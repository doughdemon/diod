Name: diod
Version: 1.0.21
Release: 1

Summary:  I/O forwarding server for 9P.
License: GPL
Group: Applications/System
# URL: http://sourceforge.net/projects/npfs
Source0: %{name}-%{version}.tar.gz
Buildroot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires: tcp_wrappers-devel
BuildRequires: lua-devel
BuildRequires: munge-devel
BuildRequires: ncurses-devel
BuildRequires: libcap-devel
BuildRequires: libibverbs-devel librdmacm-devel
BuildRequires: gperftools-devel
BuildRequires: libattr-devel attr

%description
diod is a 9P server used in combination with the kernel v9fs file
system for I/O forwarding on Linux clusters.

%prep
%setup -q

%build
%configure --with-tcmalloc
make CFLAGS="-Werror -O2 -g"

%check
make check CFLAGS=-Werror

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT
make install DESTDIR=$RPM_BUILD_ROOT

# Kludge to install diodmount as a mount helper.
mkdir -p $RPM_BUILD_ROOT/sbin
mv $RPM_BUILD_ROOT%{_sbindir}/diodmount \
   $RPM_BUILD_ROOT/sbin/mount.diod
mv $RPM_BUILD_ROOT%{_mandir}/man8/diodmount.8 \
   $RPM_BUILD_ROOT%{_mandir}/man8/mount.diod.8

%clean
rm -rf ${RPM_BUILD_ROOT}

%post
if [ -x /sbin/chkconfig ]; then /sbin/chkconfig --add diod; fi

%preun
if [ "$1" = 0 ]; then
  %{_sysconfdir}/init.d/diod stop >/dev/null 2>&1 || :
  if [ -x /sbin/chkconfig ]; then /sbin/chkconfig --del diod; fi
fi

%files
%defattr(-,root,root)
%doc AUTHORS COPYING README INSTALL ChangeLog
%{_sbindir}/*
/sbin/*
%{_mandir}/man8/*
%{_mandir}/man5/*
%attr(0755,root,root) %{_sysconfdir}/init.d/diod
%config(noreplace) %attr(0755,root,root) %{_sysconfdir}/auto.diod
%config(noreplace) %attr(0644,root,root) %{_sysconfdir}/diod.conf
