# Windows: fetch the headers and shaders this repo doesn't redistribute into third_party\
#   reshade-include\  ReShade 6.8.0 add-on API headers (crosire/reshade)
#   shaders\          ReShade.fxh, ReShadeUI.fxh, DisplayDepth.fx (+ L10N) (crosire/reshade-shaders, slim)
#   imgui\            imgui.h + imconfig.h 1.92.5 docking (what ReShade 6.8.0's overlay API expects)
# ReShade itself comes from its official installer on Windows (the version with full add-on support), see README.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'
$v = '6.8.0'
foreach ($d in 'reshade-include', 'shaders', 'imgui') { New-Item -ItemType Directory -Force -Path (Join-Path $tp $d) | Out-Null }
foreach ($f in 'reshade.hpp', 'reshade_api.hpp', 'reshade_api_device.hpp', 'reshade_api_pipeline.hpp', 'reshade_api_resource.hpp',
		'reshade_api_format.hpp', 'reshade_events.hpp', 'reshade_overlay.hpp') {
	Invoke-WebRequest "https://raw.githubusercontent.com/crosire/reshade/v$v/include/$f" -OutFile (Join-Path $tp "reshade-include\$f")
}
foreach ($f in 'ReShade.fxh', 'ReShadeUI.fxh', 'DisplayDepth.fx', 'DisplayDepth_L10N.fxh') {
	Invoke-WebRequest "https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/$f" -OutFile (Join-Path $tp "shaders\$f")
}
foreach ($f in 'imgui.h', 'imconfig.h') {
	Invoke-WebRequest "https://raw.githubusercontent.com/ocornut/imgui/v1.92.5-docking/$f" -OutFile (Join-Path $tp "imgui\$f")
}
Write-Host "fetched into $tp"
