#!/bin/bash
# pgo-build.sh <phase>   phase = gen | use | clean
# Profile-guided build of the NATIVE side of wine-ppc64le in a shadow tree
# (~/Projects/power8/wt-pgo), never in the tree the user launches from.
#   gen : rsync tree -> wt-pgo, configure with -fprofile-generate, make.
#   use : rebuild wt-pgo with -fprofile-use from the .gcda the training run wrote.
#   clean: delete wt-pgo and the profile data (standing policy: agent trees die).
set -eu
SRC=$HOME/Projects/power8/wine-ppc64le
WT=$HOME/Projects/power8/wt-pgo
PD=$HOME/pgo-data
J=${J:-72}
case "${1:?phase}" in
gen)
  rsync -a --delete --exclude /.git --exclude 'autom4te.cache' "$SRC/" "$WT/"
  mkdir -p "$PD"; rm -rf "$PD"/*
  cd "$WT"
  make -s distclean >/dev/null 2>&1 || true
  ppc64_CFLAGS="-g -O2" i386_CFLAGS="-g -O2" \
  CFLAGS="-g -O2 -fprofile-generate -fprofile-update=atomic -fprofile-dir=$PD" LDFLAGS="-fprofile-generate" \
    ./configure --enable-win64 --enable-archs=ppc64,i386 > configure.pgo-gen.log 2>&1
  # the preloader is static and -nostdlib: it cannot carry gcov, build it plain
  rm -f loader/*.o
  # makedep puts EXTRADEFS before CFLAGS, so -fno-profile-generate in Makefile.in loses to CFLAGS;
  # compile the two preloader objects by hand with the opt-out LAST (static -nostdlib binary, no gcov)
  for o in loader/preloader.o loader/preloader_mac.o; do rm -f $o; cmd=$(make -n $o 2>/dev/null | tr "\n" " " | sed "s/\\\\ / /g" | grep -o "gcc -c -o $o .*"); [ -n "$cmd" ] && eval "$cmd -fno-profile-generate"; done
  make -s -j"$J" > build.pgo-gen.log 2>&1
  echo "gen build done: $(ls -la dlls/ntdll/ntdll.so | cut -c1-60)"
  ;;
reconf)   # configure again (flags changed), then the make steps
  cd "$WT"
  make -s distclean >/dev/null 2>&1 || true
  ppc64_CFLAGS="-g -O2" i386_CFLAGS="-g -O2" \
  CFLAGS="-g -O2 -fprofile-generate -fprofile-update=atomic -fprofile-dir=$PD" LDFLAGS="-fprofile-generate" \
    ./configure --enable-win64 --enable-archs=ppc64,i386 > configure.pgo-gen.log 2>&1
  # the native PE arch copies CFLAGS (configure.ac:2510); gcov adds TLS and the PE
  # converter refuses PT_TLS, so the PE arch goes back to plain flags here.
  sed -i "s|^ppc64_CFLAGS = .*|ppc64_CFLAGS = -g -O2|; s|^ppc64_LDFLAGS = .*|ppc64_LDFLAGS =|" config.status
  ./config.status > /dev/null 2>&1
  touch config.status Makefile
  grep -c "define PACKAGE_VERSION" include/config.h
  make -s -j"$J" > build.pgo-gen.log 2>&1
  echo "gen build done: $(ls -la dlls/ntdll/ntdll.so | cut -c1-60)"
  ;;
genmake)  # re-run only the make steps of gen (configure already done)
  cd "$WT"
  rm -f loader/*.o
  # makedep puts EXTRADEFS before CFLAGS, so -fno-profile-generate in Makefile.in loses to CFLAGS;
  # compile the two preloader objects by hand with the opt-out LAST (static -nostdlib binary, no gcov)
  for o in loader/preloader.o loader/preloader_mac.o; do rm -f $o; cmd=$(make -n $o 2>/dev/null | tr "\n" " " | sed "s/\\\\ / /g" | grep -o "gcc -c -o $o .*"); [ -n "$cmd" ] && eval "$cmd -fno-profile-generate"; done
  make -s -j"$J" > build.pgo-gen.log 2>&1
  echo "gen build done: $(ls -la dlls/ntdll/ntdll.so | cut -c1-60)"
  ;;
use)
  cd "$WT"
  n=$(find "$PD" -name '*.gcda' | wc -l); echo "gcda files: $n"; [ "$n" -gt 0 ]
  make -s distclean >/dev/null 2>&1 || true
  ppc64_CFLAGS="-g -O2" i386_CFLAGS="-g -O2" \
  CFLAGS="-g -O2 -fprofile-use -fprofile-correction -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch -fprofile-dir=$PD" \
    ./configure --enable-win64 --enable-archs=ppc64,i386 > configure.pgo-use.log 2>&1
  sed -i "s|^ppc64_CFLAGS = .*|ppc64_CFLAGS = -g -O2|; s|^ppc64_LDFLAGS = .*|ppc64_LDFLAGS =|" config.status
  ./config.status > /dev/null 2>&1
  touch config.status Makefile
  rm -f loader/*.o
  for o in loader/preloader.o loader/preloader_mac.o; do rm -f $o; cmd=$(make -n $o 2>/dev/null | tr "\n" " " | sed "s/\\\\ / /g" | grep -o "gcc -c -o $o .*"); [ -n "$cmd" ] && eval "$cmd -fno-profile-use"; done
  make -s -j"$J" > build.pgo-use.log 2>&1
  echo "use build done: $(ls -la dlls/ntdll/ntdll.so | cut -c1-60)"
  ;;
clean)
  rm -rf "$WT" "$PD"; echo cleaned ;;
esac
