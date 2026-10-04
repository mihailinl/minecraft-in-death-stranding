#!/bin/bash
# Fetch what isn't redistributed here into third_party/:
#   reshade-6.8.0-addon/ReShade64.dll   ReShade with full add-on support (reshade.me), extracted from its setup exe
#   reshade-include/                    ReShade's add-on API headers (crosire/reshade v6.8.0)
#   shaders/                            ReShade.fxh, ReShadeUI.fxh, DisplayDepth.fx (+ L10N) (crosire/reshade-shaders, slim)
#   imgui/                              imgui.h + imconfig.h 1.92.5 docking (the version ReShade 6.8.0's overlay API expects)
# Needs curl and 7z (or bsdtar).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
TP="$HERE/third_party"
RESHADE=6.8.0
UA="Mozilla/5.0 (X11; Linux x86_64) Firefox/140.0"
mkdir -p "$TP/reshade-$RESHADE-addon" "$TP/reshade-include" "$TP/shaders" "$TP/imgui"

setup="$TP/reshade-$RESHADE-addon/ReShade_Setup_${RESHADE}_Addon.exe"
[ -f "$setup" ] || curl -fsSL -A "$UA" -e https://reshade.me/ -o "$setup" "https://reshade.me/downloads/ReShade_Setup_${RESHADE}_Addon.exe"
# the setup exe carries its DLLs as an appended zip
if command -v 7z >/dev/null; then
	7z e -y -o"$TP/reshade-$RESHADE-addon" "$setup" ReShade64.dll >/dev/null
else
	bsdtar -xf "$setup" -C "$TP/reshade-$RESHADE-addon" ReShade64.dll
fi

for f in reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp \
	reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp; do
	curl -fsSL "https://raw.githubusercontent.com/crosire/reshade/v$RESHADE/include/$f" -o "$TP/reshade-include/$f"
done
for f in ReShade.fxh ReShadeUI.fxh DisplayDepth.fx DisplayDepth_L10N.fxh; do
	curl -fsSL "https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/$f" -o "$TP/shaders/$f"
done
for f in imgui.h imconfig.h; do
	curl -fsSL "https://raw.githubusercontent.com/ocornut/imgui/v1.92.5-docking/$f" -o "$TP/imgui/$f"
done
ls -R "$TP" | head -30
