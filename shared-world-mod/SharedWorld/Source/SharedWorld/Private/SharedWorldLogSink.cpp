#include "SharedWorldLogSink.h"

#include "SharedWorldTypes.h"

void FSharedWorldLogSink::Write(sw::LogLevel Level, const std::string& Event, const sw::LogFields& Fields)
{
	const FString Line = UTF8_TO_TCHAR(sw::FormatLogLine(Event, Fields).c_str());
	switch (Level)
	{
	case sw::LogLevel::Debug:
		UE_LOG(LogSharedWorld, Verbose, TEXT("[SharedWorld] %s"), *Line);
		break;
	case sw::LogLevel::Info:
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] %s"), *Line);
		break;
	case sw::LogLevel::Warning:
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] %s"), *Line);
		break;
	case sw::LogLevel::Error:
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] %s"), *Line);
		break;
	}
}
