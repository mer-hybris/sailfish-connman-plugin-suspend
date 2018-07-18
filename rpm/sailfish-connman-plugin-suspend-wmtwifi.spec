Name: sailfish-connman-plugin-suspend-wmtwifi
Version: 0.0.1
Release: 1
Summary: wmtWifi connman plugin for suspend
Group: Development/Libraries
License: GPLv2
URL: https://github.com/mer-hybris/sailfish-connman-plugin-suspend-wmtwifi
Source: %{name}-%{version}.tar.bz2
Requires: connman
BuildRequires: pkgconfig(libmce-glib) >= 1.0.5
BuildRequires: pkgconfig(connman)
BuildRequires: pkgconfig(libnl-3.0)
BuildRequires: pkgconfig(libnl-genl-3.0)

%define plugin_dir %{_libdir}/connman/plugins

%description
This is a package which makes sure devices with a gen2 /dev/wmtWifi driver can
sucessfully suspend while WLAN is active.

%prep
%setup -q -n %{name}-%{version}

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release

%install
rm -rf %{buildroot}
%make_install

mkdir -p %{buildroot}/%{plugin_dir}

%files
%defattr(-,root,root,-)
%{plugin_dir}/*.so
