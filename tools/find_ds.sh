#!/bin/bash
# Print the Death Stranding Director's Cut install folder (Steam app 1850570), searching every Steam library.
for vdf in "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" "$HOME/.steam/steam/steamapps/libraryfolders.vdf" \
	"$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/libraryfolders.vdf"; do
	[ -f "$vdf" ] || continue
	for lib in $(sed -nE 's/.*"path"[[:space:]]*"([^"]+)".*/\1/p' "$vdf"); do
		d="$lib/steamapps/common/DEATH STRANDING DIRECTORS CUT"
		[ -f "$d/ds.exe" ] && { echo "$d"; exit 0; }
	done
done
echo "DEATH STRANDING DIRECTORS CUT not found in Steam libraries (set DS_DIR)" >&2
exit 1
