%global gh_commit    3b733fcb4ba60c7e4407e76932796baac9eae911
%global gh_short     %(c=%{gh_commit}; echo ${c:0:7})
%global gh_owner     Sude-
%global gh_project   lgogdownloader

Name:	lgogdownloader
Version:	20180531
Release:	0.%{gh_short}%{?dist}
Summary:	GOGDownloader for Linux

License:	WTFPL
URL:     https://github.com/%{gh_owner}/%{gh_project}
Source0: https://github.com/%{gh_owner}/%{gh_project}/archive/%{gh_commit}/%{gh_project}-%{version}-%{gh_short}.tar.gz

BuildRequires: cmake
BuildRequires: boost-devel
BuildRequires: libcurl-devel
BuildRequires: openssl-devel
BuildRequires: liboauth-devel
BuildRequires: jsoncpp-devel
BuildRequires: htmlcxx-devel
BuildRequires: rhash-devel
BuildRequires: tinyxml2-devel
BuildRequires: zlib-devel
BuildRequires: help2man
BuildRequires: gzip

%description
LGOGDownloader is open source downloader to GOG.com for Linux users using the same API as the official GOGDownloader

%prep
%autosetup -n %{gh_project}-%{gh_commit}

%build
%cmake .
%make_build


%install
%make_install


%files
%license COPYING
%doc README.md
%{_bindir}/lgogdownloader
%{_mandir}/man1/lgogdownloader.1*



%changelog

