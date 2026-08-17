#!/bin/sh
# Register (or unregister) the wine-ppc64le-native compatibility tool with the
# local Steam client.
#
#   ./install.sh              symlink this directory into compatibilitytools.d
#   ./install.sh --copy       copy it instead (use if the tree is not readable
#                             by the account Steam runs as)
#   ./install.sh --uninstall  remove the entry
#
# Steam only rescans compatibilitytools.d at startup, so the client has to be
# restarted by hand afterwards.  This script never touches a running Steam.

set -u

name=wine-ppc64le-native
src=$(cd "$(dirname "$0")" && pwd -P)

mode=link
case ${1:-} in
--copy)       mode=copy ;;
--uninstall)  mode=uninstall ;;
"")           ;;
*)            echo "usage: $0 [--copy|--uninstall]" >&2; exit 2 ;;
esac

# Steam's data root: the same candidates steam.sh itself walks.
root=
for cand in "${STEAM_ROOT:-}" "$HOME/.local/share/Steam" "$HOME/.steam/steam" \
            "$HOME/.steam/root" "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"
do
    if [ -n "$cand" ] && [ -d "$cand/steamapps" ]; then root=$cand; break; fi
done
[ -n "$root" ] || { echo "install.sh: no Steam install found" >&2; exit 1; }

dir=$root/compatibilitytools.d
dst=$dir/$name

if [ "$mode" = uninstall ]; then
    rm -rf "$dst"
    echo "removed $dst"
    exit 0
fi

mkdir -p "$dir"
rm -rf "$dst"

if [ "$mode" = link ]; then
    ln -s "$src" "$dst"
else
    mkdir -p "$dst"
    cp -f "$src/proton" "$src/compatibilitytool.vdf" "$src/toolmanifest.vdf" "$dst/"
fi
chmod +x "$src/proton" 2>/dev/null

echo "installed $dst -> $src"
echo
echo "Next steps (Steam reads compatibilitytools.d only at startup):"
echo "  1. restart the Steam client"
echo "  2. right-click the game -> Properties -> Compatibility"
echo "  3. tick 'Force the use of a specific Steam Play compatibility tool'"
echo "  4. choose 'Wine ppc64le (native)'"
