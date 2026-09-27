#include "SharedWorldCredentialStore.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincred.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	/** Credential Manager target names are one flat namespace: scope ours. */
	FString TargetName(const std::string& Key)
	{
		return FString(TEXT("SatisfactorySharedWorld:")) + UTF8_TO_TCHAR(Key.c_str());
	}
}

#if PLATFORM_WINDOWS

sw::Result<std::string> FSharedWorldCredentialStore::Read(const std::string& Key)
{
	const FString Target = TargetName(Key);
	PCREDENTIALW Cred = nullptr;
	if (!CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Cred))
	{
		const DWORD Err = GetLastError();
		if (Err == ERROR_NOT_FOUND)
		{
			return sw::MakeError(sw::ErrorCode::NotFound, "no stored credential");
		}
		return sw::MakeError(sw::ErrorCode::Io, "Windows Credential Manager read failed");
	}
	std::string Out;
	if (Cred->CredentialBlob && Cred->CredentialBlobSize > 0)
	{
		// The blob is stored as UTF-16 bytes (see Write); convert back.
		const FString Wide(Cred->CredentialBlobSize / sizeof(WCHAR), reinterpret_cast<const WCHAR*>(Cred->CredentialBlob));
		Out = TCHAR_TO_UTF8(*Wide);
	}
	CredFree(Cred);
	return Out;
}

sw::Status FSharedWorldCredentialStore::Write(const std::string& Key, const std::string& Secret)
{
	const FString Target = TargetName(Key);
	const FString Wide = UTF8_TO_TCHAR(Secret.c_str());
	CREDENTIALW Cred = {};
	Cred.Type = CRED_TYPE_GENERIC;
	Cred.TargetName = const_cast<LPWSTR>(*Target);
	Cred.CredentialBlobSize = Wide.Len() * sizeof(WCHAR);
	Cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<WCHAR*>(*Wide));
	Cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
	Cred.UserName = const_cast<LPWSTR>(TEXT("SatisfactorySharedWorld"));
	if (!CredWriteW(&Cred, 0))
	{
		return sw::MakeError(sw::ErrorCode::Io, "Windows Credential Manager write failed");
	}
	return {};
}

sw::Status FSharedWorldCredentialStore::Remove(const std::string& Key)
{
	const FString Target = TargetName(Key);
	if (!CredDeleteW(*Target, CRED_TYPE_GENERIC, 0))
	{
		const DWORD Err = GetLastError();
		if (Err == ERROR_NOT_FOUND)
		{
			return {}; // already gone: idempotent
		}
		return sw::MakeError(sw::ErrorCode::Io, "Windows Credential Manager delete failed");
	}
	return {};
}

#else // !PLATFORM_WINDOWS

sw::Result<std::string> FSharedWorldCredentialStore::Read(const std::string&)
{
	return sw::MakeError(sw::ErrorCode::Unsupported, "credential storage is not implemented on this platform");
}

sw::Status FSharedWorldCredentialStore::Write(const std::string&, const std::string&)
{
	return sw::MakeError(sw::ErrorCode::Unsupported, "credential storage is not implemented on this platform");
}

sw::Status FSharedWorldCredentialStore::Remove(const std::string&)
{
	return {}; // nothing was ever stored
}

#endif
