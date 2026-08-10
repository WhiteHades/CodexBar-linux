Name:           codexbar-linux
Version:        0.4.2.2
Release:        1%{?dist}
Summary:        Native Linux AI usage monitor

License:        MIT
URL:            https://github.com/WhiteHades/CodexBar-linux
Source0:        %{name}-%{version}.tar.gz
Packager:       WhiteHades <WhiteHades@users.noreply.github.com>

BuildRequires:  appstream
BuildRequires:  desktop-file-utils
BuildRequires:  dbus-daemon
BuildRequires:  gcc
BuildRequires:  meson >= 1.6.0
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(glib-2.0) >= 2.78
BuildRequires:  pkgconfig(gio-2.0) >= 2.78
BuildRequires:  pkgconfig(json-c) >= 0.17
BuildRequires:  pkgconfig(libcurl) >= 8.0
BuildRequires:  pkgconfig(ncursesw) >= 6.4
BuildRequires:  pkgconfig(sqlite3)

%description
CodexBar displays AI provider usage and account limits in a terminal,
desktop status item, Waybar, scripts, and JSON output.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

%check
%meson_test
desktop-file-validate \
    %{buildroot}%{_datadir}/applications/com.steipete.codexbar.desktop \
    %{buildroot}%{_sysconfdir}/xdg/autostart/codexbar-status.desktop
appstreamcli validate --no-net \
    %{buildroot}%{_datadir}/metainfo/com.steipete.codexbar.metainfo.xml

%files
%license %{_datadir}/doc/%{name}/copyright
%{_bindir}/codexbar-linux
%{_bindir}/codexbar-process-supervisor
%{_datadir}/applications/com.steipete.codexbar.desktop
%{_datadir}/icons/hicolor/scalable/apps/com.steipete.codexbar.svg
%{_datadir}/metainfo/com.steipete.codexbar.metainfo.xml
%config(noreplace) %{_sysconfdir}/xdg/autostart/codexbar-status.desktop

%changelog
* Mon Aug 10 2026 WhiteHades <WhiteHades@users.noreply.github.com> - 0.4.2.2-1
- Add Debian, RPM, AppImage, and AUR packaging

* Sat Aug 01 2026 WhiteHades <WhiteHades@users.noreply.github.com> - 0.4.1-1
- Initial native Linux package
