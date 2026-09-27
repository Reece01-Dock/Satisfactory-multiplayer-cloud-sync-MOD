#pragma once

#include "CoreMinimal.h"
#include "SharedWorldCore/Providers/GitHubAuth.h"

/**
 * Stores secrets (the GitHub OAuth token) in the Windows Credential Manager
 * (CredWriteW / CredReadW / CredDeleteW), scoped to the current OS user.
 * Never written to the save, the world repository, or any log line.
 *
 * On non-Windows targets (none currently shipped: Satisfactory is Win64-only)
 * this reports Unsupported rather than falling back to an unsafe plaintext
 * store.
 */
class FSharedWorldCredentialStore final : public sw::ICredentialStore
{
public:
	sw::Result<std::string> Read(const std::string& Key) override;
	sw::Status Write(const std::string& Key, const std::string& Secret) override;
	sw::Status Remove(const std::string& Key) override;
};
