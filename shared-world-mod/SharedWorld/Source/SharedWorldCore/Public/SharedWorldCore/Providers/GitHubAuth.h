#pragma once
// GitHub sign-in with the OAuth 2.0 device flow: the mod shows a short code,
// the player enters it at github.com/login/device, the mod polls. Only the
// app's public client id ships with the mod (no client secret). Credentials
// are handed to an ICredentialStore; they are never logged or shown.
//
// When the OAuth App has expiring tokens enabled, GitHub returns an access
// token (~8h) plus a refresh token (~6 months). This module persists both and
// refreshes transparently so the player is not asked to re-link every day.
//
// Architecture:
//   UI → GitHubAuthService → GitHubDeviceFlow + ICredentialStore + /user validation
//   GitHub API Token() → ResolveGitHubAccessToken (refresh if needed)

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "SharedWorldCore/Providers/Http.h"
#include "SharedWorldCore/Util/Log.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	/** OS-backed secret store for the GitHub OAuth credential blob. */
	class ICredentialStore
	{
	public:
		virtual ~ICredentialStore() = default;
		virtual Result<std::string> Read(const std::string& Key) = 0; // NotFound if absent
		virtual Status Write(const std::string& Key, const std::string& Secret) = 0;
		virtual Status Remove(const std::string& Key) = 0;
	};

	/** Alias matching the product architecture name. */
	using IGitHubCredentialStore = ICredentialStore;

	/** Tests only. */
	class MemoryCredentialStore final : public ICredentialStore
	{
	public:
		Result<std::string> Read(const std::string& Key) override;
		Status Write(const std::string& Key, const std::string& Secret) override;
		Status Remove(const std::string& Key) override;

	private:
		std::mutex Mutex;
		std::map<std::string, std::string> Secrets;
	};

	/**
	 * Persisted OAuth credential. Serialised as JSON in the credential store.
	 * Legacy blobs that are a bare access token (no '{') are still accepted.
	 */
	struct GitHubOAuthTokens
	{
		std::string AccessToken;
		std::string RefreshToken; // empty when the OAuth App does not issue refreshing tokens
		TimeMs AccessExpiresAt = 0;  // 0 = unknown / non-expiring
		TimeMs RefreshExpiresAt = 0; // 0 = unknown / none
		std::string Scope;
	};

	/** Sanitised auth status for diagnostics. Never includes credential values. */
	struct GitHubAuthDiagnostics
	{
		std::string Provider = "github";
		bool bAuthenticated = false;
		bool bAccessTokenPresent = false;
		bool bAccessTokenExpired = false;
		bool bRefreshTokenPresent = false;
		bool bRefreshPossible = false;
		/** Seconds until access expiry; -1 if unknown / non-expiring. */
		int64_t ExpiresInSeconds = -1;
	};

	/** Parse a credential-store blob (JSON v1 or legacy bare token). */
	Result<GitHubOAuthTokens> ParseGitHubCredentialBlob(const std::string& Blob);
	/** Serialise tokens for the credential store. Never log the result. */
	std::string SerializeGitHubCredentialBlob(const GitHubOAuthTokens& Tokens);
	/** Build a diagnostics snapshot from stored tokens + clock (no network). */
	GitHubAuthDiagnostics MakeGitHubAuthDiagnostics(const GitHubOAuthTokens& Tokens, TimeMs Now, bool bSessionConnected);

	/**
	 * Load the stored credential, refresh when expired/near-expiry (or when
	 * bForceRefresh), persist rotated tokens, and return the current access token.
	 * Concurrent callers for the same store key are serialised so refresh-token
	 * rotation cannot race.
	 */
	Result<std::string> ResolveGitHubAccessToken(std::shared_ptr<IHttpClient> Http, std::shared_ptr<ICredentialStore> Store,
		const IClock& Clock, const std::string& ClientId, const std::string& CredentialKey, bool bForceRefresh = false,
		Logger Log = Logger{}, std::string WebBase = "https://github.com");

	struct DeviceCode
	{
		std::string Code;            // device_code (secret-ish; never shown)
		std::string UserCode;        // shown to the player, e.g. WDJB-MJHT
		std::string VerificationUri; // https://github.com/login/device
		int IntervalSeconds = 5;
		TimeMs ExpiresAt = 0;
	};

	enum class DevicePoll { Pending, SlowDown, Authorized, Denied, Expired };

	struct DevicePollResult
	{
		DevicePoll State = DevicePoll::Pending;
		GitHubOAuthTokens Tokens; // only when Authorized
		int IntervalSeconds = 5;  // updated on SlowDown

		/** Convenience for older call sites / tests. */
		std::string& AccessToken() { return Tokens.AccessToken; }
		const std::string& AccessToken() const { return Tokens.AccessToken; }
	};

	struct GitHubUserInfo
	{
		std::string Login;
		std::string Id;        // numeric id as decimal string
		std::string Name;      // display name; may be empty
		std::string AvatarUrl; // may be empty
	};

	/** Low-level GitHub device authorization + user lookup + token refresh HTTP client. */
	class GitHubDeviceFlow
	{
	public:
		GitHubDeviceFlow(std::shared_ptr<IHttpClient> InHttp, std::string InClientId, const IClock& InClock, std::string InScope = "repo",
			std::string InWebBase = "https://github.com", std::string InApiBase = "https://api.github.com");

		Result<DeviceCode> Start();
		/** One poll; call again after IntervalSeconds while Pending / SlowDown. */
		Result<DevicePollResult> Poll(const DeviceCode& Code);
		/** Exchange a refresh token for a new access (+ rotated refresh) token set. */
		Result<GitHubOAuthTokens> Refresh(const std::string& RefreshToken);
		/** GitHub login of the token's owner (GET /user). */
		Result<std::string> FetchLogin(const std::string& Token);
		/** Full profile used for Connected state. */
		Result<GitHubUserInfo> FetchUser(const std::string& Token);

	private:
		std::shared_ptr<IHttpClient> Http;
		std::string ClientId;
		const IClock& Clock;
		std::string Scope;
		std::string WebBase;
		std::string ApiBase;
	};

	enum class GitHubAuthState
	{
		Disconnected,
		Starting,
		WaitingForUser,
		Authorizing,
		Connected,
		Expired,
		Denied,
		Error,
	};

	enum class GitHubAuthErrorKind
	{
		None,
		ClientIdMissing,
		NoInternet,
		GitHubUnreachable,
		DeviceCodeExpired,
		AuthorizationDenied,
		TokenInvalid,
		TokenRevoked,
		RateLimited,
		MalformedResponse,
		Cancelled,
		CredentialStoreFailed,
		Other,
	};

	struct GitHubAuthSnapshot
	{
		GitHubAuthState State = GitHubAuthState::Disconnected;
		GitHubAuthErrorKind Error = GitHubAuthErrorKind::None;
		std::string UserCode;
		std::string VerificationUri;
		int IntervalSeconds = 5;
		GitHubUserInfo User;
		std::string PlayerMessage;
		bool bHasToken = false;
		GitHubAuthDiagnostics Diagnostics;
	};

	/**
	 * Resolves the public OAuth App client id.
	 * Priority: EnvValue (if non-empty) → Embedded (if non-empty) → empty.
	 * Packaged builds should bake Embedded via SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED.
	 */
	std::string ResolveGitHubOAuthClientId(const char* Embedded, const char* EnvValue);

	/** Short player-facing string for the given state/error. Never includes tokens. */
	std::string GitHubAuthPlayerMessage(GitHubAuthState State, GitHubAuthErrorKind Error, const GitHubUserInfo& User = {});

	/**
	 * Owns device authorization, credential persistence, and connection validation.
	 * HTTP polling is driven by the caller (async worker); this class never blocks
	 * itself on sleeps — Call BeginLink, then PollOnce until terminal, then Finalize.
	 */
	class GitHubAuthService
	{
	public:
		GitHubAuthService(std::shared_ptr<IHttpClient> Http, std::shared_ptr<ICredentialStore> Store, const IClock& Clock,
			std::string ClientId, std::string CredentialKey, Logger Log = Logger{});

		bool IsConfigured() const;
		const std::string& ClientId() const { return ClientIdValue; }

		GitHubAuthSnapshot Snapshot() const;
		/** Sanitised credential status (reads store; no network). */
		GitHubAuthDiagnostics Diagnostics() const;

		/** Start device authorization. On success state is WaitingForUser with user code + URI. */
		Status BeginLink();
		/** One token poll. Pending/SlowDown keep WaitingForUser; Authorized → Authorizing. */
		Result<DevicePollResult> PollOnce();
		/** After Authorized: validate token, persist (incl. refresh), set Connected. */
		Status FinalizeAuthorized(GitHubOAuthTokens Tokens);
		/** Stop an in-progress link (caller must stop its poll loop). */
		void CancelLink();
		/** Delete local credential + clear cached user. Does not call GitHub revoke APIs. */
		Status Disconnect();
		/**
		 * On startup: if a credential is stored, refresh when needed and validate.
		 * Connected on success; only removes credentials when recovery is impossible.
		 */
		Status RestoreSession();
		/** Developer/debug: GET /user with a valid access token (refreshing if needed). */
		Result<GitHubUserInfo> TestAccess();
		/** Ensure a usable access token is available (refresh if needed). */
		Result<std::string> EnsureValidAccessToken(bool bForceRefresh = false);

		/** Classify a transport/API error for UI without exposing payloads. */
		static GitHubAuthErrorKind ClassifyError(const Error& Err);

	private:
		void SetState(GitHubAuthState State, GitHubAuthErrorKind Error = GitHubAuthErrorKind::None);
		Status PersistAndConnect(const GitHubOAuthTokens& Tokens, GitHubUserInfo User);
		Result<GitHubOAuthTokens> LoadTokens() const;
		Status SaveTokens(const GitHubOAuthTokens& Tokens);
		GitHubAuthDiagnostics DiagnosticsUnlocked(TimeMs Now) const;

		std::shared_ptr<IHttpClient> Http;
		std::shared_ptr<ICredentialStore> Store;
		const IClock& Clock;
		std::string ClientIdValue;
		std::string CredentialKey;
		Logger Log;
		mutable std::mutex Mutex;
		GitHubAuthState State = GitHubAuthState::Disconnected;
		GitHubAuthErrorKind ErrorKind = GitHubAuthErrorKind::None;
		DeviceCode ActiveCode;
		GitHubUserInfo User;
		bool bHasToken = false;
		std::atomic<uint64_t> LinkGeneration{0};
	};
}
