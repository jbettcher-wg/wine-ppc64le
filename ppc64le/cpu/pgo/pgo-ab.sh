#!/bin/bash
# PGO A/B: A = the user's launch tree (plain -O2), B = wt-pgo (profile-use build of the
# unix side), Cyberpunk -benchmark, two legs each, A A B B, same env as the HWTSO runs.
export FEX_CODECACHESCOPE=all FEX_ENABLECODECACHINGWIP=1 FEX_HOSTPAGEMODE=force MESA_SHADER_CACHE_MAX_SIZE=10G
unset MANGOHUD GALLIUM_HUD
MAIN=$HOME/Projects/power8/wine-ppc64le/ppc64le/cpu
PGO=$HOME/Projects/power8/wt-pgo/ppc64le/cpu
LOG=$MAIN/bench-cp2077-results.txt
( cd $MAIN && BENCH_NOTE="plain-O2,main-tree" ./bench-cp2077.sh 2 pgo-A-plain )
( cd $PGO && LOG=$LOG BENCH_NOTE="pgo-use,wt-pgo" ./bench-cp2077.sh 2 pgo-B-pgo )
echo PGO-AB-DONE
