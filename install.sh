#!/bin/bash
# Put ReShade (add-on build) and this mod's files into the Death Stranding DC folder, or take them out again.
#   ./install.sh            install (records every file it adds in installed.txt)
#   ./install.sh --remove   delete exactly what install.sh added, plus the files ReShade writes itself
# Steam launch options the mod needs:  WINEDLLOVERRIDES="dxgi=n,b" %command%
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
GAME=${DS_DIR:-$("$HERE/tools/find_ds.sh")}
MANIFEST="$HERE/installed.txt"
TP="$HERE/third_party"

[ -f "$GAME/ds.exe" ] || { echo "ds.exe not found in $GAME (set DS_DIR)"; exit 1; }

if [ "$1" = --remove ]; then
	[ -f "$MANIFEST" ] || { echo "nothing recorded in $MANIFEST"; exit 0; }
	while IFS= read -r rel; do rm -f -- "$GAME/$rel"; done < "$MANIFEST"
	# written by ReShade at runtime, never by the game
	rm -f -- "$GAME/ReShade.log" "$GAME/ReShade.log1"
	rmdir -p "$GAME/reshade-shaders/Shaders" "$GAME/reshade-shaders/Textures" 2>/dev/null || true
	rm -f "$MANIFEST"
	echo "removed; game folder is back to stock (ls below)"
	ls "$GAME"
	exit 0
fi

# never clobber a dxgi.dll we didn't put there
if [ -f "$GAME/dxgi.dll" ] && ! grep -qx dxgi.dll "$MANIFEST" 2>/dev/null; then
	echo "$GAME/dxgi.dll exists and isn't ours; refusing"; exit 1
fi

: > "$MANIFEST.new"
put() { # put <source> <path relative to the game folder>
	echo "$2" >> "$MANIFEST.new"
	cmp -s "$1" "$GAME/$2" && return
	mkdir -p "$(dirname "$GAME/$2")"
	# new inode + rename: a running game keeps its mapping of the old DLL instead of crashing on a rewrite
	cp -f "$1" "$GAME/$2.new" && mv -f "$GAME/$2.new" "$GAME/$2"
}

put "$TP/reshade-6.8.0-addon/ReShade64.dll" dxgi.dll
# ReShade's screenshots go to this repo's shots/ (a Windows path under Wine: Z: is /)
mkdir -p "$HERE/shots"
SHOTS_WIN="Z:$(printf '%s' "$HERE/shots" | tr '/' '\\\\')"
mkdir -p "$HERE/ds/build"
sed "s|@SHOTS@|${SHOTS_WIN//\\/\\\\}|" "$HERE/ds/ReShade.ini" > "$HERE/ds/build/ReShade.ini"   # sed needs \\ for \
put "$HERE/ds/build/ReShade.ini" ReShade.ini
put "$HERE/ds/ReShadePreset.ini" ReShadePreset.ini
for f in ReShade.fxh ReShadeUI.fxh DisplayDepth.fx DisplayDepth_L10N.fxh; do put "$TP/shaders/$f" "reshade-shaders/Shaders/$f"; done
for f in "$HERE"/ds/shaders/*.fx; do [ -f "$f" ] && put "$f" "reshade-shaders/Shaders/$(basename "$f")"; done
for f in "$HERE"/ds/build/*.addon64; do [ -f "$f" ] && put "$f" "$(basename "$f")"; done

sort -u "$MANIFEST.new" > "$MANIFEST"; rm -f "$MANIFEST.new"
echo "installed into $GAME:"; sed 's/^/  /' "$MANIFEST"
echo 'Steam > DEATH STRANDING DC > Properties > Launch options:  WINEDLLOVERRIDES="dxgi=n,b" %command%'
