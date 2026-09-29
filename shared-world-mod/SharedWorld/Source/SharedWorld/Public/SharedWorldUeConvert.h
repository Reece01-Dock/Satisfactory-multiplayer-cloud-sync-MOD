#pragma once

#include "CoreMinimal.h"
#include <string>

/**
 * FString <-> std::string helpers shared by SharedWorld UE glue.
 * Must NOT live in anonymous namespaces inside .cpp files: Unreal unity builds
 * merge those TUs and duplicate the helpers.
 */
namespace SharedWorldUe
{
	inline std::string Std(const FString& S)
	{
		return TCHAR_TO_UTF8(*S);
	}

	inline FString ToFString(const std::string& S)
	{
		return UTF8_TO_TCHAR(S.c_str());
	}
}
