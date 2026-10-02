<#
  Builds librclone.dll (rclone's official C-callable engine) from the rclone source and installs it into the mod.

  rclone is written in Go, so there is no C++ port. librclone is the supported way to embed it: the same source, built
  with `go build --buildmode=c-shared`, exposing RcloneInitialize / RcloneRPC / RcloneFreeString / RcloneFinalize.
  The mod loads the DLL at run time (see Source/SharedWorld/Private/Rclone/RcloneRuntime.cpp); nothing links to it.

  Needs on the BUILD machine only (players just receive the finished DLL inside the mod):
    - git
    - Go (the version named in rclone's go.mod)  https://go.dev/dl/
    - MinGW-w64 gcc on PATH (cgo)                 MSYS2 -> pacman -S mingw-w64-ucrt-x86_64-gcc, add C:\msys64\ucrt64\bin to PATH

  Usage:
    .\build-librclone.ps1                       # latest rclone release, installs into shared-world-mod\SharedWorld
    .\build-librclone.ps1 -Tag v1.68.2          # pin a release
    .\build-librclone.ps1 -PluginDir 'E:\SatisfactoryModding\SatisfactoryModLoader\Mods\SharedWorld','E:\...\shared-world-mod\SharedWorld'
#>
param(
	[string]$Tag = '',
	[string[]]$PluginDir = @(),
	[string]$WorkDir = (Join-Path $PSScriptRoot 'work')
)

$ErrorActionPreference = 'Stop'

function Need([string]$Name, [string]$Hint)
{
	if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) { throw "$Name was not found on PATH. $Hint" }
}
Need 'git' 'Install Git for Windows.'
Need 'go'  'Install Go from https://go.dev/dl/ (the version listed in rclone''s go.mod).'
Need 'gcc' 'Install MSYS2 (https://www.msys2.org), run: pacman -S mingw-w64-ucrt-x86_64-gcc, then add C:\msys64\ucrt64\bin to PATH.'

if (-not $PluginDir -or $PluginDir.Count -eq 0)
{
	$PluginDir = @((Resolve-Path (Join-Path $PSScriptRoot '..\..\shared-world-mod\SharedWorld')).Path)
}

if (-not $Tag)
{
	$release = Invoke-RestMethod -Uri 'https://api.github.com/repos/rclone/rclone/releases/latest' -Headers @{ 'User-Agent' = 'shared-world-build' }
	$Tag = $release.tag_name
}
Write-Host "Building librclone from rclone $Tag"

$src = Join-Path $WorkDir 'rclone-src'
if (Test-Path $src) { Remove-Item $src -Recurse -Force }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
git clone --depth 1 --branch $Tag https://github.com/rclone/rclone.git $src
if ($LASTEXITCODE -ne 0) { throw "git clone of rclone $Tag failed" }

$out = Join-Path $WorkDir 'librclone.dll'
Push-Location $src
try
{
	$env:CGO_ENABLED = '1'
	go build --buildmode=c-shared -trimpath -ldflags '-s -w' -o $out github.com/rclone/rclone/librclone
	if ($LASTEXITCODE -ne 0) { throw 'go build failed' }
}
finally { Pop-Location }

$hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
foreach ($dir in $PluginDir)
{
	$dest = Join-Path $dir 'Binaries\ThirdParty\rclone'
	New-Item -ItemType Directory -Force -Path $dest | Out-Null
	Copy-Item $out (Join-Path $dest 'librclone.dll') -Force
	Copy-Item (Join-Path $src 'COPYING') (Join-Path $dest 'rclone-LICENSE.txt') -Force   # rclone is MIT licensed: ship the notice
	"$Tag`n$hash" | Set-Content (Join-Path $dest 'rclone-version.txt')
	Write-Host "Installed -> $dest"
}
Write-Host "Done. rclone $Tag, sha256 $hash"
