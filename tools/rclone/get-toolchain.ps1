<#
  Downloads a PORTABLE Go + MinGW-w64 gcc into tools\rclone\work\toolchain (gitignored) so librclone can be built
  without installing anything system-wide and without admin rights. Every download is checksum-verified:
    - Go:       sha256 published at https://go.dev/dl/?mode=json
    - MinGW-w64 (winlibs, https://github.com/brechtsanders/winlibs_mingw): the .sha256 published beside the release zip

  build-librclone.ps1 picks the toolchain up automatically when it exists.
#>
param(
	[string]$Root = (Join-Path $PSScriptRoot 'work\toolchain')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force -Path $Root | Out-Null

function Get-Verified([string]$Url, [string]$OutFile, [string]$ExpectedSha256)
{
	Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $OutFile
	$actual = (Get-FileHash $OutFile -Algorithm SHA256).Hash.ToLower()
	if ($actual -ne $ExpectedSha256.ToLower())
	{
		Remove-Item -LiteralPath $OutFile -Force
		throw "Checksum mismatch for $Url (expected $ExpectedSha256, got $actual)"
	}
}

# ---- Go
$goDir = Join-Path $Root 'go'
if (-not (Test-Path (Join-Path $goDir 'bin\go.exe')))
{
	$releases = Invoke-RestMethod 'https://go.dev/dl/?mode=json'
	$file = $releases[0].files | Where-Object { $_.os -eq 'windows' -and $_.arch -eq 'amd64' -and $_.kind -eq 'archive' } | Select-Object -First 1
	Write-Host "Go $($releases[0].version): $($file.filename)"
	$zip = Join-Path $Root 'go.zip'
	Get-Verified "https://go.dev/dl/$($file.filename)" $zip $file.sha256
	$tmp = Join-Path $Root 'go-extract'
	Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
	if (Test-Path -LiteralPath $goDir) { Remove-Item -LiteralPath $goDir -Recurse -Force }
	Move-Item -LiteralPath (Join-Path $tmp 'go') -Destination $goDir
	Remove-Item -LiteralPath $tmp -Recurse -Force
	Remove-Item -LiteralPath $zip -Force
}
Write-Host "Go ready: $goDir"

# ---- MinGW-w64 gcc (needed by cgo to produce a DLL)
$gccDir = Join-Path $Root 'winlibs'
if (-not (Test-Path (Join-Path $gccDir 'mingw64\bin\gcc.exe')))
{
	$release = Invoke-RestMethod 'https://api.github.com/repos/brechtsanders/winlibs_mingw/releases/latest' -Headers @{ 'User-Agent' = 'shared-world-build' }
	$asset = $release.assets | Where-Object { $_.name -match '^winlibs-x86_64-posix-seh-gcc-.*-mingw-w64ucrt-.*\.zip$' } | Select-Object -First 1
	$shaAsset = $release.assets | Where-Object { $_.name -eq ($asset.name + '.sha256') } | Select-Object -First 1
	if (-not $shaAsset) { throw "No published checksum for $($asset.name); refusing to use an unverified compiler." }
	Write-Host "MinGW-w64: $($asset.name)"
	$shaResponse = Invoke-WebRequest -UseBasicParsing -Uri $shaAsset.browser_download_url
	$shaText = if ($shaResponse.Content -is [byte[]]) { [System.Text.Encoding]::UTF8.GetString($shaResponse.Content) } else { [string]$shaResponse.Content }
	$expected = ($shaText.Trim() -split '\s+')[0]
	if ($expected -notmatch '^[0-9a-fA-F]{64}$') { throw "Unexpected checksum file format: $shaText" }
	$zip = Join-Path $Root 'winlibs.zip'
	Get-Verified $asset.browser_download_url $zip $expected
	if (Test-Path -LiteralPath $gccDir) { Remove-Item -LiteralPath $gccDir -Recurse -Force }
	Expand-Archive -LiteralPath $zip -DestinationPath $gccDir -Force
	Remove-Item -LiteralPath $zip -Force
}
Write-Host "gcc ready: $gccDir"
Write-Host "TOOLCHAIN_OK"
