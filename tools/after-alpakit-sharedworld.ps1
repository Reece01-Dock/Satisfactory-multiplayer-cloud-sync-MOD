# Called after Alpakit finishes packaging SharedWorld.
# Bumps the patch version and writes Desktop\SharedWorlds.zip for ficsit.app.
#
# One-time Alpakit setup (Editor):
#   Alpakit -> After Package / custom command:
#   powershell -NoProfile -ExecutionPolicy Bypass -File "E:\Documents\Satisfactory-multiplayer-cloud-sync-MOD\tools\after-alpakit-sharedworld.ps1"
#
# Or run manually after Alpakit:
#   powershell -NoProfile -File tools\after-alpakit-sharedworld.ps1

param(
	[ValidateSet('Patch', 'Minor', 'Major')]
	[string]$Bump = 'Patch'
)

$ErrorActionPreference = 'Stop'
$package = Join-Path $PSScriptRoot 'package-smr.ps1'
$zip = 'E:\SatisfactoryModding\SatisfactoryModLoader\Saved\ArchivedPlugins\SharedWorld\SharedWorld-Windows.zip'

if (-not (Test-Path $zip)) {
	Write-Error "Alpakit Windows zip not found: $zip`nPackage SharedWorld with Alpakit first."
	exit 1
}

$args = @{}
switch ($Bump) {
	'Minor' { $args.Minor = $true }
	'Major' { $args.Major = $true }
}

Write-Host "=== SharedWorlds auto package ($Bump bump) ==="
& $package @args
exit $LASTEXITCODE
