#pragma once
// Structured logging: an event name plus key/value fields, e.g.
//   [SharedWorld] event=LeaseAcquired world=our-factory generation=592 revision=184
// Values of keys that look like secrets are replaced by [REDACTED] before
// any sink sees them.

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sw
{
	enum class LogLevel { Debug, Info, Warning, Error };

	using LogFields = std::vector<std::pair<std::string, std::string>>;

	class ILogSink
	{
	public:
		virtual ~ILogSink() = default;
		virtual void Write(LogLevel Level, const std::string& Event, const LogFields& Fields) = 0;
	};

	/** True for keys such as token, secret, password, authorization, credential, refresh_token. */
	bool IsSecretKey(const std::string& Key);
	/** "event=X k=v k2=\"v v\"" with secrets redacted. */
	std::string FormatLogLine(const std::string& Event, const LogFields& Fields);

	class Logger
	{
	public:
		explicit Logger(std::shared_ptr<ILogSink> InSink = nullptr) : Sink(std::move(InSink)) {}

		void Log(LogLevel Level, const std::string& Event, LogFields Fields = {}) const;
		void Debug(const std::string& Event, LogFields Fields = {}) const { Log(LogLevel::Debug, Event, std::move(Fields)); }
		void Info(const std::string& Event, LogFields Fields = {}) const { Log(LogLevel::Info, Event, std::move(Fields)); }
		void Warn(const std::string& Event, LogFields Fields = {}) const { Log(LogLevel::Warning, Event, std::move(Fields)); }
		void Error(const std::string& Event, LogFields Fields = {}) const { Log(LogLevel::Error, Event, std::move(Fields)); }

	private:
		std::shared_ptr<ILogSink> Sink;
	};

	/** Keeps lines in memory (tests, diagnostics screen). */
	class MemoryLogSink final : public ILogSink
	{
	public:
		/** MaxLines > 0 keeps only the newest lines (the in-game diagnostics ring). */
		explicit MemoryLogSink(size_t InMaxLines = 0) : MaxLines(InMaxLines) {}
		void Write(LogLevel Level, const std::string& Event, const LogFields& Fields) override;
		std::vector<std::string> Lines() const;
		bool Contains(const std::string& Needle) const;

	private:
		mutable std::mutex Mutex;
		std::vector<std::string> Stored;
		size_t MaxLines;
	};

	/** Writes to stderr. */
	class StderrLogSink final : public ILogSink
	{
	public:
		void Write(LogLevel Level, const std::string& Event, const LogFields& Fields) override;
	};
}
