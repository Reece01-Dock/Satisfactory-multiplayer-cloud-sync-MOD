#include "SharedWorldCore/Util/Log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace sw
{
	bool IsSecretKey(const std::string& Key)
	{
		std::string K = Key;
		std::transform(K.begin(), K.end(), K.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
		for (const char* S : {"token", "secret", "password", "authorization", "credential", "refresh", "apikey", "api_key", "cookie", "private"})
		{
			if (K.find(S) != std::string::npos)
			{
				return true;
			}
		}
		return false;
	}

	std::string FormatLogLine(const std::string& Event, const LogFields& Fields)
	{
		std::string Out = "event=" + Event;
		for (const auto& [K, V] : Fields)
		{
			const std::string Value = IsSecretKey(K) ? std::string("[REDACTED]") : V;
			Out += ' ';
			Out += K;
			Out += '=';
			const bool bQuote = Value.empty() || Value.find_first_of(" \"=") != std::string::npos;
			if (bQuote)
			{
				Out += '"';
				for (char C : Value)
				{
					if (C == '"' || C == '\\') Out += '\\';
					Out += (C == '\n' || C == '\r') ? ' ' : C;
				}
				Out += '"';
			}
			else
			{
				Out += Value;
			}
		}
		return Out;
	}

	void Logger::Log(LogLevel Level, const std::string& Event, LogFields Fields) const
	{
		if (!Sink)
		{
			return;
		}
		for (auto& [K, V] : Fields)
		{
			if (IsSecretKey(K))
			{
				V = "[REDACTED]";
			}
		}
		Sink->Write(Level, Event, Fields);
	}

	void MemoryLogSink::Write(LogLevel, const std::string& Event, const LogFields& Fields)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Stored.push_back(FormatLogLine(Event, Fields));
		if (MaxLines > 0 && Stored.size() > MaxLines + MaxLines / 4)
		{
			// Trim in batches so a busy log does not shift the vector on every line.
			Stored.erase(Stored.begin(), Stored.end() - static_cast<std::ptrdiff_t>(MaxLines));
		}
	}

	std::vector<std::string> MemoryLogSink::Lines() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Stored;
	}

	bool MemoryLogSink::Contains(const std::string& Needle) const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		for (const std::string& L : Stored)
		{
			if (L.find(Needle) != std::string::npos)
			{
				return true;
			}
		}
		return false;
	}

	void StderrLogSink::Write(LogLevel Level, const std::string& Event, const LogFields& Fields)
	{
		static const char* Names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
		std::fprintf(stderr, "[SharedWorld] %s %s\n", Names[static_cast<int>(Level)], FormatLogLine(Event, Fields).c_str());
	}
}
