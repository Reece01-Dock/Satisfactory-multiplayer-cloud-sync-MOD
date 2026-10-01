# Bump SharedWorld.uplugin SemVersion (and matching Version / VersionName / RemoteVersionRange).
# Only touches TOP-LEVEL fields (never Plugins[].SemVersion).
#
# Usage:
#   .\tools\bump-version.ps1              # 1.0.0 -> 1.0.1 (patch)
#   .\tools\bump-version.ps1 -Minor       # 1.0.1 -> 1.1.0
#   .\tools\bump-version.ps1 -Major       # 1.1.0 -> 2.0.0
#   .\tools\bump-version.ps1 -Set 1.2.3
#   .\tools\bump-version.ps1 -NoSync

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

$raw = Get-Content -LiteralPath $Uplugin -Raw

# Prefer top-level SemVersion (line that is only that field, not Plugins entries).
if ($raw -notmatch '(?m)^\s*"SemVersion"\s*:\s*"([^"]+)"\s*,?\s*$') {
	throw "Could not find top-level SemVersion in $Uplugin"
}
$old = $Matches[1]

if ($Set) {
	$new = $Set.Trim()
} else {
	$core = ($old -split '-', 2)[0]
	$parts = @($core -split '\.')
	while ($parts.Count -lt 3) { $parts += '0' }
	[int]$maj = $parts[0]
	[int]$min = $parts[1]
	[int]$pat = $parts[2]
	if ($Major) { $maj++; $min = 0; $pat = 0 }
	elseif ($Minor) { $min++; $pat = 0 }
	else { $pat++ }
	$new = "$maj.$min.$pat"
}

if ($new -notmatch '^\d+\.\d+\.\d+(\-[0-9A-Za-z\.-]+)?$') {
	throw "Invalid semver '$new'"
}

$majorInt = [int](($new -split '\.')[0])
$remoteRange = ">=$new <$($majorInt + 1).0.0"

function Set-TopLevelField([string]$Text, [string]$Name, [string]$Value, [switch]$AsNumber) {
	# Top-level only: field alone on its line (Plugins SemVersion sits mid-line).
	if ($AsNumber) {
		$pattern = '(?m)^(\s*"' + [regex]::Escape($Name) + '"\s*:\s*)\d+(\s*,?\s*)$'
		$replacement = '${1}' + $Value + '${2}'
	} else {
		$pattern = '(?m)^(\s*"' + [regex]::Escape($Name) + '"\s*:\s*")[^"]*("\s*,?\s*)$'
		$replacement = '${1}' + $Value.Replace('${', '$${') + '${2}'
	}

	# String overload with count=1 (NOT RegexOptions).
	$updated = [regex]::Replace($Text, $pattern, $replacement, 1)
	if ($updated -eq $Text) {
		# Already correct?
		if ($AsNumber) {
			if ($Text -match ('(?m)^\s*"' + [regex]::Escape($Name) + '"\s*:\s*' + [regex]::Escape($Value) + '\b')) {
				return $Text
			}
		} elseif ($Text -match ('(?m)^\s*"' + [regex]::Escape($Name) + '"\s*:\s*"' + [regex]::Escape($Value) + '"')) {
			return $Text
		}
		throw "Failed to update top-level field $Name"
	}
	return $updated
}

$raw = Set-TopLevelField $raw 'Version' "$majorInt" -AsNumber
$raw = Set-TopLevelField $raw 'VersionName' $new
$raw = Set-TopLevelField $raw 'SemVersion' $new
$raw = Set-TopLevelField $raw 'RemoteVersionRange' $remoteRange

# Restore SML dependency if a previous buggy bump overwrote it.
$raw = [regex]::Replace(
	$raw,
	'(?m)(\{\s*"Name"\s*:\s*"SML"\s*,\s*"Enabled"\s*:\s*true\s*,\s*"SemVersion"\s*:\s*")[^"]*(")',
	'${1}^3.12.0${2}',
	1
)

$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($Uplugin, $raw.TrimEnd() + "`n", $utf8NoBom)
Write-Host "Version $old -> $new (Version=$majorInt, RemoteVersionRange=$remoteRange)"

if (-not $NoSync -and (Test-Path (Split-Path $SmlUplugin -Parent))) {
	Copy-Item -LiteralPath $Uplugin -Destination $SmlUplugin -Force
	Write-Host "Synced to SML Mods: $SmlUplugin"
}

Write-Output $new
