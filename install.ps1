# Windows: put this mod's files into the Death Stranding Director's Cut folder, or take them out again.
#   powershell -ExecutionPolicy Bypass -File install.ps1            install
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Remove    remove exactly what it added
#   ... -GameDir "D:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT"   if it isn't found
# ReShade must already be installed there with its official installer (the version with full add-on support,
# DirectX 10/11/12): that puts dxgi.dll and a ReShade.ini in the game folder.
param([switch]$Remove, [string]$GameDir = $env:DS_DIR)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$manifest = Join-Path $root 'installed.txt'
$name = 'DEATH STRANDING DIRECTORS CUT'

if (-not $GameDir) {
	$steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
	$libs = @()
	if ($steam) {
		$vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
		if (Test-Path $vdf) {
			$libs = Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
		}
		$libs += $steam
	}
	foreach ($l in $libs) {
		$d = Join-Path $l "steamapps\common\$name"
		if (Test-Path (Join-Path $d 'ds.exe')) { $GameDir = $d; break }
	}
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir 'ds.exe'))) { throw "ds.exe not found: pass -GameDir" }

if ($Remove) {
	if (Test-Path $manifest) {
		Get-Content $manifest | ForEach-Object { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $GameDir $_) }
		Remove-Item $manifest
	}
	$bak = Join-Path $GameDir 'ReShade.ini.dsmc-backup'
	if (Test-Path $bak) { Move-Item -Force $bak (Join-Path $GameDir 'ReShade.ini') }
	Write-Host "removed from $GameDir (ReShade itself stays; uninstall it with its installer)"
	exit 0
}

if (-not (Test-Path (Join-Path $GameDir 'dxgi.dll'))) {
	Write-Warning "no dxgi.dll in $GameDir: install ReShade (with full add-on support) for ds.exe first"
}
$addon = Join-Path $root 'ds\build\dsmc.addon64'
if (-not (Test-Path $addon)) { throw "build ds\build\dsmc.addon64 first (ds\build.bat)" }

$added = New-Object System.Collections.Generic.List[string]
function Put([string]$src, [string]$rel) {
	$dst = Join-Path $GameDir $rel
	New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
	Copy-Item -Force $src $dst
	$added.Add($rel)
}

# our ReShade.ini (depth copy settings, keys, screenshot folder); the installer's one is kept as a backup
$ini = Join-Path $GameDir 'ReShade.ini'
$bak = Join-Path $GameDir 'ReShade.ini.dsmc-backup'
if ((Test-Path $ini) -and -not (Test-Path $bak)) { Copy-Item $ini $bak }
$shots = Join-Path $root 'shots'
New-Item -ItemType Directory -Force -Path $shots | Out-Null
(Get-Content (Join-Path $root 'ds\ReShade.ini')) -replace '@SHOTS@', $shots | Set-Content -Encoding ASCII $ini

Put $addon 'dsmc.addon64'
Put (Join-Path $root 'ds\ReShadePreset.ini') 'ReShadePreset.ini'
foreach ($f in 'ReShade.fxh', 'ReShadeUI.fxh', 'DisplayDepth.fx', 'DisplayDepth_L10N.fxh') {
	Put (Join-Path $root "third_party\shaders\$f") "reshade-shaders\Shaders\$f"
}
Get-ChildItem (Join-Path $root 'ds\shaders\*.fx') | ForEach-Object { Put $_.FullName "reshade-shaders\Shaders\$($_.Name)" }
$added | Sort-Object -Unique | Set-Content $manifest
Write-Host "installed into ${GameDir}:"
$added | ForEach-Object { Write-Host "  $_" }
