# GitHub OAuth setup (Shared Worlds)

Shared Worlds links a player's GitHub account with the **OAuth 2.0 device authorization flow**. Players never use GitHub CLI, PATs, SSH keys, or Git config.

## What players see

1. Shared Worlds → **Link GitHub**
2. Browser opens to GitHub's device page (or they open it manually)
3. They enter the short code shown in-game
4. Approve access on GitHub
5. Return to the game → **Connected as \<username\>**

Tokens are stored in the **Windows Credential Manager** (never in `settings.json`, saves, logs, or the world repo).

## Create a GitHub OAuth App

1. GitHub → **Settings** → **Developer settings** → **OAuth Apps** → **New OAuth App**
2. Application name: e.g. `Shared Worlds`
3. Homepage URL: your project / mod page URL
4. Authorization callback URL: use a placeholder such as `http://127.0.0.1/` (device flow does not redirect here)
5. Enable **Device Flow** on the OAuth App (GitHub → OAuth App settings → Device Flow)
6. Copy the **Client ID** (public). Do **not** put a Client Secret in the mod.

Recommended scopes for world storage: `repo` (private repository access).

## Where the Client ID is configured

Priority at runtime:

1. Environment variable `SHAREDWORLD_GITHUB_CLIENT_ID` (developers / local testing)
2. Compile-time embedded value `SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED` (packaged builds)

### Developers (local)

```powershell
$env:SHAREDWORLD_GITHUB_CLIENT_ID = "Iv1.your_public_client_id"
```

Or set `SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED` when building Unreal so you do not need the env var every launch:

```powershell
$env:SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED = "Iv1.your_public_client_id"
# then build the SharedWorld module as usual
```

`SharedWorld.Build.cs` reads `SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED` from the environment and defines it for the compile.

### Packaged / CI builds

The build pipeline must supply the public Client ID so end users never set an environment variable:

- Set `SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED` in the CI/build environment before compiling the mod, **or**
- Define `SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED` in the Unreal target / Build.cs for release configurations.

The Client ID is **not a secret**. Never commit a Client Secret. Never log access tokens.

If the Client ID is missing in a build, the UI shows:

> GitHub integration is not configured in this build.

## How to test device login

1. Configure a Client ID (env or embedded).
2. Launch Satisfactory with the mod.
3. Open Shared Worlds → Settings (or Welcome) → **Link GitHub**.
4. Confirm the browser opens `https://github.com/login/device` and the in-game code matches.
5. Approve on GitHub; the UI should show **Connected as \<you\>**.
6. Restart the game; connection should restore without logging in again.
7. **Disconnect** removes the Windows credential and returns to Not connected.
8. Optional: **Test Connection** (Settings) calls `GET /user` only — it does not create commits or push branches.

## Developer Git vs in-mod OAuth

- **Repository development** (clone/push this project) may continue to use HTTPS + Git Credential Manager / `gh`. That path is separate.
- **In-mod linking** uses `GitHubAuthService` + device flow + OS credential store only. It must not require `gh auth login`.

## Security checklist

- No tokens in plaintext JSON, `.ini`, project files, saves, or logs
- Logs may include username and `token_present=yes|no`, never the token value
- Disconnect deletes the local OS credential; remote GitHub app revocation is a separate optional GitHub UI action (the mod does not claim remote revocation unless implemented)
