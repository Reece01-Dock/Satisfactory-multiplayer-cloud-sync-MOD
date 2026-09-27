#include "SharedWorldCore/Util/Result.h"

namespace sw
{
	const char* ToString(ErrorCode Code)
	{
		switch (Code)
		{
		case ErrorCode::Invalid: return "Invalid";
		case ErrorCode::NotFound: return "NotFound";
		case ErrorCode::AlreadyExists: return "AlreadyExists";
		case ErrorCode::Conflict: return "Conflict";
		case ErrorCode::Fenced: return "Fenced";
		case ErrorCode::StaleRevision: return "StaleRevision";
		case ErrorCode::NoWorld: return "NoWorld";
		case ErrorCode::Contention: return "Contention";
		case ErrorCode::Io: return "Io";
		case ErrorCode::Network: return "Network";
		case ErrorCode::Corrupt: return "Corrupt";
		case ErrorCode::Unauthorized: return "Unauthorized";
		case ErrorCode::RateLimited: return "RateLimited";
		case ErrorCode::Unsupported: return "Unsupported";
		case ErrorCode::Cancelled: return "Cancelled";
		case ErrorCode::BadState: return "BadState";
		case ErrorCode::Ambiguous: return "Ambiguous";
		}
		return "Unknown";
	}
}
