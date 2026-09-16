#!/bin/bash
# pgo-train.sh -- one Cyberpunk -benchmark leg out of the INSTRUMENTED shadow
# tree (wt-pgo), so its ntdll.so/win32u.so/winecom/d3d11 write .gcda profiles
# into ~/pgo-data.  The wt-pgo copy of bench-cp2077.sh derives TOOL and
# WINE_PPC64LE_TREE from its own location, which is what points the launch at
# the shadow tree.  Also one leg of the crossing microbench for the cold paths.
set -u
export FEX_CODECACHESCOPE=all FEX_ENABLECODECACHINGWIP=1 FEX_HOSTPAGEMODE=force MESA_SHADER_CACHE_MAX_SIZE=10G
unset MANGOHUD GALLIUM_HUD
WT=$HOME/Projects/power8/wt-pgo
echo "gcda before: $(find $HOME/pgo-data -name '*.gcda' 2>/dev/null | wc -l)"
cd "$WT/ppc64le/cpu" && BENCH_NOTE="pgo-instrumented-training" ./bench-cp2077.sh 1 pgo-train
sleep 20
echo "gcda after game: $(find $HOME/pgo-data -name '*.gcda' | wc -l)"
ls "$WT/ppc64le/cpu/bench-crossing.sh" >/dev/null 2>&1 && WINE_PPC64LE_TREE=$WT timeout 600 "$WT/ppc64le/cpu/bench-crossing.sh" 2>&1 | tail -4
echo "gcda after bench: $(find $HOME/pgo-data -name '*.gcda' | wc -l)"
du -sh $HOME/pgo-data | cut -f1
echo TRAIN-DONE
