Name:           ps_mem-c
Version:        %{pkg_version}
Release:        1%{?dist}
Summary:        C port of ps_mem with smaps_rollup optimization
License:        LGPLv2
URL:            https://github.com/rushikeshsakharleofficial/ps_mem-c
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc

%description
C port of ps_mem that uses /proc/<pid>/smaps_rollup (Linux 4.4+)
for 2.2x speedup over the original C binary and faster than Python v3.14.
Falls back to full smaps on older kernels.

%prep
%setup -q

%build
gcc %{optflags} -o ps_mem ps_mem.c

%install
install -Dm755 ps_mem       %{buildroot}%{_bindir}/ps_mem
install -Dm644 README.md    %{buildroot}%{_docdir}/%{name}/README.md

%files
%{_bindir}/ps_mem
%doc README.md

%changelog
* Tue May 20 2026 Rushikesh Sakharle <ramsharath@instantly.ai> - %{pkg_version}-1
- Release %{pkg_version}
