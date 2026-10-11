#!/bin/bash
# Print the install folder of Death Stranding Director's Cut (Steam app 1850570), or with "2" Death Stranding 2:
# On the Beach, searching every Steam library.
if [ "$1" = 2 ]; then
	dir="DEATH STRANDING 2 - ON THE BEACH"; exe=DS2.exe
else
	dir="DEATH STRANDING DIRECTORS CUT"; exe=ds.exe
fi
for vdf in "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" "$HOME/.steam/steam/steamapps/libraryfolders.vdf" \
	"$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/libraryfolders.vdf"; do
	[ -f "$vdf" ] || continue
	for lib in $(sed -nE 's/.*"path"[[:space:]]*"([^"]+)".*/\1/p' "$vdf"); do
		d="$lib/steamapps/common/$dir"
		[ -f "$d/$exe" ] && { echo "$d"; exit 0; }
	done
done
echo "$dir not found in Steam libraries (set DS_DIR)" >&2
exit 1
