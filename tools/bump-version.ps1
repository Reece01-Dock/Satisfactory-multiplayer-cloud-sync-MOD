# Bump SharedWorld.uplugin SemVersion (and matching Version / VersionName / RemoteVersionRange).
# Usage:
#   .\tools\bump-version.ps1              # 1.0.0 -> 1.0.1 (patch)
#   .\tools\bump-version.ps1 -Minor       # 1.0.1 -> 1.1.0
#   .\tools\bump-version.ps1 -Major       # 1.1.0 -> 2.0.0
#   .\tools\bump-version.ps1 -Set 1.2.3
#   .\tools\bump-version.ps1 -NoSync      # don't copy into SML Mods

param(
	[switch]$Major,
	[switch]$Minor,
	[string]$Set,
	[switch]$NoSync
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent
$Uplugin = Join-Path $RepoRoot 'shared-world-mod\SharedWorld\SharedWorld.uplugin'
$SmlUplugin = 'E:\SatisfactoryModding\SatisfactoryModLoader\Mods\SharedWorld\SharedWorld.uplugin'

if (-not (Test-Path $Uplugin)) { throw "Missing $Uplugin" }

$raw = Get-Content $Uplugin -Raw
if ($raw -notmatch '"SemVersion"\s*:\s*"([^"]+)"') {
	throw "Could not find SemVersion in $Uplugin"
}
$old = $Matches[1]

if ($Set) {
	$new = $Set
} else {
	$parts = $old.Split('-')[0].Split('.')
	while ($parts.Count -lt 3) { $parts += '0' }
	[int]$maj = $parts[0]; [int]$min = $parts[1]; [int]$pat = $parts[2]
	if ($Major) { $maj++; $min = 0; $pat = 0 }
	elseif ($Minor) { $min++; $pat = 0 }
	else { $pat++ }
	$new = "$maj.$min.$pat"
}

if ($new -notmatch '^\d+\.\d+\.\d+') {
	throw "Invalid semver '$new' (expected Major.Minor.Patch)"
}
$majorInt = [int]($new.Split('.')[0])
$nextMajor = $majorInt + 1
$remoteRange = ">=$new <$nextMajor.0.0"

function Set-JsonField([string]$Text, [string]$Name, [string]$Value, [switch]$Number) {
	$pattern = if ($Number) {
		'("' + [regex]::Escape($Name) + '"\s*:\s*)\d+'
	} else {
		'("' + [regex]::Escape($Name) + '"\s*:\s*")[^"]*(")'
	}
	$replacement = if ($Number) { "`${1}$Value" } else { "`${1}$Value`${2}" }
	$updated = [regex]::Replace($Text, $pattern, $replacement, 1)
	if ($updated -eq $Text) { throw "Failed to update field $Name" }
	return $updated
}

$raw = Set-JsonField $raw 'Version' "$majorInt" -Number
$raw = Set-JsonField $raw 'VersionName' $new
$raw = Set-JsonField $raw 'SemVersion' $new
$raw = Set-JsonField $raw 'RemoteVersionRange' $remoteRange

[System.IO.File]::WriteAllText($Uplugin, $raw.TrimEnd() + "`n")
Write-Host "Version $old -> $new (Version=$majorInt, RemoteVersionRange=$remoteRange)"

if (-not $NoSync -and (Test-Path (Split-Path $SmlUplugin -Parent))) {
	Copy-Item $Uplugin $SmlUplugin -Force
	Write-Host "Synced to SML Mods: $SmlUplugin"
}

# Expose for callers
$script:BumpedVersion = $new
return $new
