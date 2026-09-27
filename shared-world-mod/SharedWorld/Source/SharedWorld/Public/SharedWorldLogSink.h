#pragma once

#include "CoreMinimal.h"
#include "SharedWorldCore/Util/Log.h"

/**
 * Routes SharedWorldCore's structured log lines to UE_LOG(LogSharedWorld).
 * Secrets are already redacted by sw::FormatLogLine before this is called.
 */
class FSharedWorldLogSink final : public sw::ILogSink
{
public:
	void Write(sw::LogLevel Level, const std::string& Event, const sw::LogFields& Fields) override;
};
