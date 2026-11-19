# RPM spec for perl5-mt (openSUSE / SUSE / Fedora)
# Build options:
#   --with pgo       Enable portable PGO + LTO + symbol binding + 2 MiB text
#   --with native    Enable host-native CPU tuning (-Dusenative)
#   --without check  Skip post-build verification gates

%bcond_with pgo
%bcond_with native
%bcond_without check

%define install_prefix /opt/perl5-mt

Name:           perl5-mt
Version:        5.40.2
Release:        1%{?dist}
Summary:        Multi-threaded Perl 5 interpreter with frozen images, fibers, and SIMD
License:        Artistic-1.0 OR GPL-1.0-or-later
URL:            https://github.com/Perl/perl5
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  glibc-devel
BuildRequires:  perl-base

# Installed under /opt/perl5-mt; do not auto-provide system perl modules
AutoReqProv:    no

%description
perl5-mt is a multi-threaded Perl 5.40.2 runtime providing immutable frozen
boot images, per-worker interpreter compartments, cooperative fibers,
deterministic numerics, and portable SIMD acceleration.

%prep
%setup -q

%build
CFG_OPTS="-des -Dusethreads -Ud_thread_local -Dprefix=%{install_prefix} -Accflags=-std=gnu17"
%if %{with pgo}
CFG_OPTS="$CFG_OPTS -Dusepgo -Dusehugepagetext"
%else
CFG_OPTS="$CFG_OPTS -Doptimize='-O2 -g'"
%endif
%if %{with native}
CFG_OPTS="$CFG_OPTS -Dusenative"
%endif

eval ./Configure $CFG_OPTS
%if %{with pgo}
make %{?_smp_mflags} pgo
%else
make %{?_smp_mflags}
%endif

%check
%if %{with check}
./perl -I. -Ilib t/mt/build_flags.t
if [ -d krt ]; then
    make -C krt test
fi
if [ -f t/mt/simd_parity.t ]; then
    PERL_FORCE_SCALAR=0 ./perl -I. -Ilib t/mt/simd_parity.t
    PERL_FORCE_SCALAR=1 ./perl -I. -Ilib t/mt/simd_parity.t
fi
if [ -f t/mt/num_digest.t ]; then
    PERL_FORCE_SCALAR=0 ./perl -I. -Ilib t/mt/num_digest.t
    PERL_FORCE_SCALAR=1 ./perl -I. -Ilib t/mt/num_digest.t
fi
%endif

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}
%if %{with pgo}
make pgo-clean || true
%endif

%files
%defattr(-,root,root,-)
%{install_prefix}

%changelog
* Wed Nov 18 2026 perl5-mt packagers <maintainers@perl5-mt.local> - 5.40.2-1
- Initial perl5-mt package with optional PGO, native tuning, and hugepage text
