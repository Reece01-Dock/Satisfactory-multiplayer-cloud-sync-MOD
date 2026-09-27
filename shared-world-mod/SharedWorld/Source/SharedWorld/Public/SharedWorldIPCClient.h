#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UObject/Object.h"
#include "SharedWorldIPCClient.generated.h"

/** Called with (bSuccess, HttpStatus, parsed JSON body or null). Always on the game thread. */
DECLARE_DELEGATE_ThreeParams(FSharedWorldIPCResponse, bool, int32, TSharedPtr<FJsonObject>);

/**
 * Talks to the local helper over its authenticated 127.0.0.1 HTTP API.
 *
 * Finds the helper through <LocalAppData>/SatisfactorySharedWorld/discovery.json
 * (port + bearer token, written by the helper and readable only by this OS
 * user). If no helper answers, it starts the bundled helper executable once
 * and retries. The token is kept in memory only and never logged.
 */
UCLASS()
class SHAREDWORLD_API USharedWorldIPCClient : public UObject
{
	GENERATED_BODY()

public:
	/** Expected helper API version (GET /v1/health -> apiVersion). */
	static constexpr int32 ExpectedApiVersion = 1;

	/** Starts connecting (and launching the helper if needed). Safe to call repeatedly. */
	void Connect();

	bool IsConnected() const { return bConnected; }
	bool IsConnecting() const { return bConnecting; }
	/** Human-readable reason when not connected. */
	const FString& GetLastError() const { return LastError; }

	/** Sends a request. Path starts with "/v1/". Body may be null. */
	void Request(const FString& Verb, const FString& Path, const TSharedPtr<FJsonObject>& Body, FSharedWorldIPCResponse OnDone);

	/** Directory holding discovery.json, config.json and the helper logs. */
	static FString GetHelperDataDir();

private:
	bool LoadDiscovery();
	bool LaunchHelper();
	void CheckHealth();
	void ScheduleRetry();
	void OnDisconnected(const FString& Reason);

	FString BaseUrl;
	FString Token;
	bool bConnected = false;
	bool bConnecting = false;
	bool bLaunchedHelper = false;
	int32 Attempts = 0;
	FString LastError;
};
