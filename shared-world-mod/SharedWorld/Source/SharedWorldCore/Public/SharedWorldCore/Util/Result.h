#pragma once
// Error handling without exceptions (Unreal builds with exceptions off).

#include <string>
#include <utility>
#include <variant>

namespace sw
{
	enum class ErrorCode
	{
		Invalid,        // bad input or bad remote data
		NotFound,
		AlreadyExists,
		Conflict,       // lost a compare-and-swap race; re-read and retry
		Fenced,         // our generation is no longer current
		StaleRevision,  // a newer revision exists
		NoWorld,
		Contention,     // CAS kept failing; safe to retry later
		Io,
		Network,
		Corrupt,        // data failed verification
		Unauthorized,
		RateLimited,
		Unsupported,
		Cancelled,
		BadState,       // operation not valid in the current state
		Ambiguous,      // outcome unknown (e.g. response lost)
	};

	const char* ToString(ErrorCode Code);

	struct Error
	{
		ErrorCode Code = ErrorCode::Invalid;
		std::string Message;

		Error() = default;
		Error(ErrorCode InCode, std::string InMessage) : Code(InCode), Message(std::move(InMessage)) {}

		/** "Fenced: lease lost ..." */
		std::string Describe() const { return std::string(ToString(Code)) + ": " + Message; }
		/** Prefixes context while keeping the code. */
		Error Wrap(const std::string& Context) const { return Error(Code, Context + ": " + Message); }
	};

	inline Error MakeError(ErrorCode Code, std::string Message) { return Error(Code, std::move(Message)); }

	template <typename T>
	class [[nodiscard]] Result
	{
	public:
		Result(T InValue) : Storage(std::in_place_index<0>, std::move(InValue)) {}
		Result(Error InError) : Storage(std::in_place_index<1>, std::move(InError)) {}

		bool Ok() const { return Storage.index() == 0; }
		explicit operator bool() const { return Ok(); }

		T& Value() & { return std::get<0>(Storage); }
		const T& Value() const& { return std::get<0>(Storage); }
		T Value() && { return std::get<0>(std::move(Storage)); }
		T* operator->() { return &std::get<0>(Storage); }
		const T* operator->() const { return &std::get<0>(Storage); }
		T& operator*() { return std::get<0>(Storage); }

		const Error& Err() const { return std::get<1>(Storage); }
		bool Is(ErrorCode Code) const { return !Ok() && Err().Code == Code; }

	private:
		std::variant<T, Error> Storage;
	};

	/** Result carrying no value. */
	class [[nodiscard]] Status
	{
	public:
		Status() = default;
		Status(Error InError) : Failure(std::move(InError)), bFailed(true) {}

		static Status Success() { return Status(); }

		bool Ok() const { return !bFailed; }
		explicit operator bool() const { return Ok(); }
		const Error& Err() const { return Failure; }
		bool Is(ErrorCode Code) const { return bFailed && Failure.Code == Code; }

	private:
		Error Failure;
		bool bFailed = false;
	};
}

// Propagation helpers. SW_TRY evaluates a Status/Result expression and returns
// its error from the enclosing function (which must return Status or Result<U>).
#define SW_CONCAT_INNER(a, b) a##b
#define SW_CONCAT(a, b) SW_CONCAT_INNER(a, b)
#define SW_TRY(expr)                                   \
	do                                                 \
	{                                                  \
		auto&& SW_CONCAT(sw_r_, __LINE__) = (expr);    \
		if (!SW_CONCAT(sw_r_, __LINE__).Ok())          \
			return SW_CONCAT(sw_r_, __LINE__).Err();   \
	} while (0)

// SW_ASSIGN(lhs, expr): evaluate a Result expression; on error return it from
// the enclosing function, otherwise move the value into lhs.
#define SW_ASSIGN(lhs, expr)                                   \
	auto SW_CONCAT(sw_a_, __LINE__) = (expr);                  \
	if (!SW_CONCAT(sw_a_, __LINE__).Ok())                      \
		return SW_CONCAT(sw_a_, __LINE__).Err();               \
	lhs = std::move(SW_CONCAT(sw_a_, __LINE__)).Value()
