Name: sailfish-connman-plugin-suspend
Version: 0.0.1
Release: 1
Summary: A wowlan and wmtWifi connman plugin for suspend
Group: Development/Libraries
License: GPLv2
URL: https://github.com/mer-hybris/sailfish-connman-plugin-suspend
Source: %{name}-%{version}.tar.bz2
Source1: 70-wifi-powersave-rules.ini
Requires: connman
BuildRequires: pkgconfig(libmce-glib) >= 1.0.5
BuildRequires: pkgconfig(connman)
BuildRequires: pkgconfig(libnl-3.0)
BuildRequires: pkgconfig(libnl-genl-3.0)
Provides: sailfish-connman-plugin-suspend-wmtwifi

%define plugin_dir %{_libdir}/connman/plugins

%description
This is a package which makes sure devices with wowlan and optionally 
a gen2/gen3 /dev/wmtWifi driver can successfully suspend while WLAN is active.

%prep
%setup -q -n %{name}-%{version}

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/%{plugin_dir}
make DESTDIR=%{buildroot} LIBDIR=%{_libdir} install
mkdir -p %{buildroot}/lib/udev/rules.d
install -m 664 %{SOURCE1} %{buildroot}/lib/udev/rules.d/70-wifi-powersave.rules

%files
%defattr(-,root,root,-)
%{plugin_dir}/*.so
/lib/udev/rules.d/70-wifi-powersave.rules
