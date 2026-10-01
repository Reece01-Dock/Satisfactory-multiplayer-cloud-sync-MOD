# After Alpakit has produced SharedWorld-Windows.zip, bump the mod version and
# build the multi-target SharedWorlds.zip that ficsit.app / SMR accepts.
#
# Usage:
#   .\tools\package-smr.ps1              # bump patch, build zip -> Desktop
#   .\tools\package-smr.ps1 -NoBump      # reuse current version
#   .\tools\package-smr.ps1 -Minor
#
# Upload Desktop\SharedWorlds.zip (never SharedWorld-Windows.zip).

param(
	[switch]$NoBump,
	[switch]$Major,
	[switch]$Minor,
	[string]$Set
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$RepoRoot = Split-Path $PSScriptRoot -Parent
$Uplugin = Join-Path $RepoRoot 'shared-world-mod\SharedWorld\SharedWorld.uplugin'
$SrcZip = 'E:\SatisfactoryModding\SatisfactoryModLoader\Saved\ArchivedPlugins\SharedWorld\SharedWorld-Windows.zip'
$ModRef = 'SharedWorlds'  # ficsit.app Mod Reference (cannot change)

if (-not (Test-Path $SrcZip)) {
	throw @"
Missing Alpakit Windows package:
  $SrcZip

Package SharedWorld with Alpakit (Shipping / Windows) first, then re-run this script.
"@
}

if (-not $NoBump) {
	$bumpArgs = @{}
	if ($Major) { $bumpArgs.Major = $true }
	elseif ($Minor) { $bumpArgs.Minor = $true }
	if ($Set) { $bumpArgs.Set = $Set }
	& (Join-Path $PSScriptRoot 'bump-version.ps1') @bumpArgs | Out-Null
}

$pluginText = Get-Content $Uplugin -Raw
if ($pluginText -notmatch '"SemVersion"\s*:\s*"([^"]+)"') {
	throw "SemVersion missing in $Uplugin"
}
$sem = $Matches[1]
Write-Host "Packaging SMR zip for $ModRef $sem"

$stage = Join-Path $env:TEMP "SharedWorlds-smr-$sem"
$modFiles = Join-Path $stage '_mod'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $modFiles | Out-Null

[System.IO.Compression.ZipFile]::ExtractToDirectory($SrcZip, $modFiles)
Get-ChildItem $modFiles -Recurse -Filter '*.pdb' | Remove-Item -Force
Remove-Item (Join-Path $modFiles 'SharedWorld.uplugin') -Force -ErrorAction SilentlyContinue
# SMR looks for <ModReference>.uplugin under each target folder
[System.IO.File]::WriteAllText((Join-Path $modFiles "$ModRef.uplugin"), $pluginText.TrimEnd() + "`n")

$desk = [Environment]::GetFolderPath('Desktop')
$distDir = Join-Path $RepoRoot 'dist'
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$outName = "$ModRef.zip"
$outDesk = Join-Path $desk $outName
$outDist = Join-Path $distDir $outName
Remove-Item $outDesk, $outDist -Force -ErrorAction SilentlyContinue

$zipStream = [System.IO.File]::Open($outDist, [System.IO.FileMode]::Create)
$zip = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Create)
try {
	foreach ($target in @('Windows', 'WindowsServer', 'LinuxServer')) {
		Get-ChildItem $modFiles -Recurse -File | ForEach-Object {
			$rel = $_.FullName.Substring($modFiles.Length).TrimStart('\', '/') -replace '\\', '/'
			$entry = $zip.CreateEntry("$target/$rel", [System.IO.Compression.CompressionLevel]::Optimal)
			$es = $entry.Open()
			try {
				$bytes = [System.IO.File]::ReadAllBytes($_.FullName)
				$es.Write($bytes, 0, $bytes.Length)
			} finally { $es.Dispose() }
		}
	}
} finally {
	$zip.Dispose()
	$zipStream.Dispose()
}

Copy-Item $outDist $outDesk -Force

# Quick validate
$check = [System.IO.Compression.ZipFile]::OpenRead($outDist)
try {
	$plugins = @($check.Entries | Where-Object { $_.FullName -like '*.uplugin' } | ForEach-Object { $_.FullName })
	$bad = @($check.Entries | Where-Object { $_.FullName -like '*\*' })
	if ($plugins.Count -ne 3) { throw "Expected 3 uplugins, found $($plugins.Count): $($plugins -join ', ')" }
	if ($bad.Count -gt 0) { throw "Zip has backslash paths (SMR will reject)" }
	Write-Host "OK: $($plugins -join ', ')"
} finally { $check.Dispose() }

$mb = [math]::Round((Get-Item $outDesk).Length / 1MB, 2)
Write-Host ""
Write-Host "Upload this file to ficsit.app -> SharedWorlds -> New Version:"
Write-Host "  $outDesk  ($mb MB)  version $sem"
