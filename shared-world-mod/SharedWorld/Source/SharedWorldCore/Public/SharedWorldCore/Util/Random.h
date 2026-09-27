#pragma once
// Random identifiers (lease nonces, world ids, operation ids).

#include <mutex>
#include <random>
#include <string>

namespace sw
{
	class IRandom
	{
	public:
		virtual ~IRandom() = default;
		/** N random bytes as lowercase hex (2N characters). */
		virtual std::string Hex(size_t Bytes) = 0;
	};

	/** std::random_device seeded; random_device is a CSPRNG on Windows/Linux toolchains Unreal uses. */
	class SystemRandom final : public IRandom
	{
	public:
		std::string Hex(size_t Bytes) override;

	private:
		std::mutex Mutex;
		std::random_device Device;
	};

	/** Deterministic, for tests. */
	class SeededRandom final : public IRandom
	{
	public:
		explicit SeededRandom(uint64_t Seed) : Engine(Seed) {}
		std::string Hex(size_t Bytes) override;

	private:
		std::mutex Mutex;
		std::mt19937_64 Engine;
	};

	/** RFC 4122 version-4 UUID string from 16 random bytes. */
	std::string NewUuid(IRandom& Rng);
}
