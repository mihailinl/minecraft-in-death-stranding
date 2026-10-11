#!/bin/bash
# Put ReShade (add-on build) and this mod's files into the Death Stranding DC folder, or take them out again.
#   ./install.sh                  install (records every file it adds in installed.txt)
#   ./install.sh --remove         delete exactly what install.sh added, plus the files ReShade writes itself
#   ./install.sh --ds2 ...        the same for Death Stranding 2 (installed-ds2.txt); support for it is being built:
#   ./install.sh --ds2 --reshade-only   only ReShade and its depth view (DisplayDepth), no add-on
# Steam launch options the mod needs:  WINEDLLOVERRIDES="dxgi=n,b" %command%
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TP="$HERE/third_party"
GAME_ID=1; RESHADE_ONLY=0; REMOVE=0
for a in "$@"; do
	case "$a" in
		--ds2) GAME_ID=2 ;;
		--reshade-only) RESHADE_ONLY=1 ;;
		--remove) REMOVE=1 ;;
		*) echo "unknown option $a"; exit 1 ;;
	esac
done
if [ $GAME_ID = 2 ]; then
	GAME=${DS_DIR:-$("$HERE/tools/find_ds.sh" 2)}; EXE=DS2.exe; MANIFEST="$HERE/installed-ds2.txt"
else
	GAME=${DS_DIR:-$("$HERE/tools/find_ds.sh")}; EXE=ds.exe; MANIFEST="$HERE/installed.txt"
fi

[ -f "$GAME/$EXE" ] || { echo "$EXE not found in $GAME (set DS_DIR)"; exit 1; }

if [ $REMOVE = 1 ]; then
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
# ReShade compiles effects with d3dcompiler_47.dll. Death Stranding DC ships Microsoft's; Death Stranding 2 doesn't,
# and Proton's built-in one (vkd3d-shader) can't compile MCPassthrough.fx ("Unhandled attribute 'fastopt'"): use
# Microsoft's, from Death Stranding DC's folder (or set D3DCOMPILER to one)
if [ ! -f "$GAME/d3dcompiler_47.dll" ] || grep -qx d3dcompiler_47.dll "$MANIFEST" 2>/dev/null; then
	DC=${D3DCOMPILER:-}
	if [ -z "$DC" ]; then
		ds1=$("$HERE/tools/find_ds.sh" 2>/dev/null || true)
		[ -n "$ds1" ] && [ -f "$ds1/d3dcompiler_47.dll" ] && DC="$ds1/d3dcompiler_47.dll"
	fi
	if [ -n "$DC" ] && [ -f "$DC" ]; then
		put "$DC" d3dcompiler_47.dll
	else
		echo "note: no Microsoft d3dcompiler_47.dll found (set D3DCOMPILER=<path>): MCPassthrough.fx may not compile"
	fi
fi
# ReShade's screenshots go to this repo's shots/ (a Windows path under Wine: Z: is /)
mkdir -p "$HERE/shots"
SHOTS_WIN="Z:$(printf '%s' "$HERE/shots" | tr '/' '\\\\')"
mkdir -p "$HERE/ds/build"
sed "s|@SHOTS@|${SHOTS_WIN//\\/\\\\}|" "$HERE/ds/ReShade.ini" > "$HERE/ds/build/ReShade.ini"   # sed needs \\ for \
put "$HERE/ds/build/ReShade.ini" ReShade.ini
for f in ReShade.fxh ReShadeUI.fxh DisplayDepth.fx DisplayDepth_L10N.fxh; do put "$TP/shaders/$f" "reshade-shaders/Shaders/$f"; done
if [ $RESHADE_ONLY = 1 ]; then
	# just the depth view: the first check in a new game is whether ReShade sees its depth buffer
	echo "Techniques=DisplayDepth@DisplayDepth.fx" > "$HERE/ds/build/ReShadePreset-depth.ini"
	put "$HERE/ds/build/ReShadePreset-depth.ini" ReShadePreset.ini
else
	put "$HERE/ds/ReShadePreset.ini" ReShadePreset.ini
	for f in "$HERE"/ds/shaders/*.fx; do [ -f "$f" ] && put "$f" "reshade-shaders/Shaders/$(basename "$f")"; done
	for f in "$HERE"/ds/build/*.addon64; do [ -f "$f" ] && put "$f" "$(basename "$f")"; done
fi

sort -u "$MANIFEST.new" > "$MANIFEST"; rm -f "$MANIFEST.new"
echo "installed into $GAME:"; sed 's/^/  /' "$MANIFEST"
echo "Steam > $( [ $GAME_ID = 2 ] && echo 'DEATH STRANDING 2' || echo 'DEATH STRANDING DC') > Properties > Launch options:  WINEDLLOVERRIDES=\"dxgi=n,b\" %command%"
