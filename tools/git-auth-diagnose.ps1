# Shared Worlds Git authentication diagnostics
# Safe: does not print tokens, does not modify the repo, does not force-push.
# Usage: powershell -File tools/git-auth-diagnose.ps1

$ErrorActionPreference = 'Continue'
Write-Host "Git authentication"
Write-Host "------------------"

function Has-Cmd($Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

$GitOk = Has-Cmd git
Write-Host ("Git installed: " + ($(if ($GitOk) { "YES" } else { "NO" })))
if (-not $GitOk) {
    Write-Host ""
    Write-Host "Action required:"
    Write-Host "Install Git for Windows: https://git-scm.com/download/win"
    exit 2
}

$GhOk = Has-Cmd gh
Write-Host ("GitHub CLI installed: " + ($(if ($GhOk) { "YES" } else { "NO" })))

$Remote = (git remote get-url origin 2>$null)
if (-not $Remote) {
    Write-Host "Remote protocol: UNKNOWN"
    Write-Host "Category: remote URL malformed / missing"
    exit 3
}

$Protocol = if ($Remote -match '^git@|^ssh://') { 'SSH' } elseif ($Remote -match '^https://') { 'HTTPS' } else { 'OTHER' }
Write-Host ("Remote protocol: " + $Protocol)
Write-Host ("Remote URL: " + $Remote)

$Helper = (git config --get credential.helper 2>$null)
if (-not $Helper) { $Helper = "(none)" }
Write-Host ("Credential helper: " + $Helper)

if ($GhOk) {
    $Auth = gh auth status 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
        Write-Host "GitHub CLI account: authenticated"
    } else {
        Write-Host "GitHub CLI account: NOT AUTHENTICATED"
        Write-Host "Category: authentication missing"
        Write-Host ""
        Write-Host "Action required:"
        Write-Host "  gh auth login"
    }
} else {
    Write-Host "GitHub CLI account: n/a (gh not installed)"
}

Write-Host -NoNewline "Repository access (ls-remote): "
$Ls = git ls-remote origin HEAD 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "OK"
    Write-Host "Fetch test: PASS"
} else {
    Write-Host "FAIL"
    Write-Host "Category: authentication missing / repository unavailable / network failure"
    Write-Host ($Ls | Out-String).Trim()
    Write-Host ""
    Write-Host "Action required:"
    if ($Protocol -eq 'HTTPS') {
        Write-Host "  Prefer: install GitHub CLI and run  gh auth login"
        Write-Host "  Or sign in via Git Credential Manager when Git prompts."
        Write-Host "  Do not store GitHub passwords. Prefer browser/device auth or a PAT in the OS credential store."
    } else {
        Write-Host "  Ensure your SSH key is loaded (ssh-add) and added to GitHub."
        Write-Host "  Test with: ssh -T git@github.com"
    }
    exit 4
}

# Push permission probe without writing commits: ask GitHub API via gh if available.
if ($GhOk) {
    Write-Host -NoNewline "Push permission: "
    $RepoPath = $Remote -replace '.*github.com[:/]', '' -replace '\.git$', ''
    $Meta = gh api "repos/$RepoPath" --jq ".permissions.push" 2>$null
    if ($LASTEXITCODE -eq 0 -and $Meta -eq 'true') {
        Write-Host "PASS"
    } elseif ($LASTEXITCODE -eq 0) {
        Write-Host "READ-ONLY (no push)"
        Write-Host "Category: no push permission"
    } else {
        Write-Host "UNKNOWN (could not query)"
    }
} else {
    Write-Host "Push permission: UNKNOWN (install gh for a safe check)"
}

Write-Host ""
Write-Host "Mod GitHub OAuth (Shared Worlds storage sign-in):"
$EnvId = [Environment]::GetEnvironmentVariable('SHAREDWORLD_GITHUB_CLIENT_ID')
if ([string]::IsNullOrWhiteSpace($EnvId)) {
    Write-Host "  SHAREDWORLD_GITHUB_CLIENT_ID: NOT SET"
    Write-Host "  Category: authentication missing (mod device-flow client id)"
    Write-Host "  Action: register a GitHub OAuth App (device flow) and set the env var for local builds."
} else {
    Write-Host "  SHAREDWORLD_GITHUB_CLIENT_ID: SET (value not printed)"
}

exit 0
