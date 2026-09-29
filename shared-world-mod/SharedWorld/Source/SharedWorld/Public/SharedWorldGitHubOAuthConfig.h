#pragma once
// Public OAuth App client id for GitHub device authorization.
//
// Packaged builds ship this public Client ID so players never set an environment
// variable. Developers may still override at runtime with
// SHAREDWORLD_GITHUB_CLIENT_ID, or at build time with
// SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED.
//
// Never embed a client secret. Device flow uses a public client.

#ifndef SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED
// Public Client ID for the Shared Worlds GitHub OAuth App (device flow).
#define SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED "Ov23liRoJ9Q0vOXQeUUe"
#endif
