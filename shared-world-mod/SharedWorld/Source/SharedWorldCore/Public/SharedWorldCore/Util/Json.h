#pragma once
// Small strict JSON library (RFC 8259) without exceptions.
//
// Remote metadata is untrusted: the parser enforces a nesting-depth and size
// limit, rejects duplicate keys, invalid UTF-8 and trailing data, and keeps
// integers exact (int64) so generations and revisions never pass through a
// double. Objects keep keys sorted, so serialisation is deterministic.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "SharedWorldCore/Util/Result.h"

namespace sw::json
{
	class Value;
	using Array = std::vector<Value>;
	using Object = std::map<std::string, Value, std::less<>>;

	enum class Type { Null, Bool, Int, Double, String, List, Map };

	class Value
	{
	public:
		Value() = default;
		Value(std::nullptr_t) {}
		Value(bool B) : Kind(Type::Bool), BoolValue(B) {}
		Value(int I) : Kind(Type::Int), IntValue(I) {}
		Value(int64_t I) : Kind(Type::Int), IntValue(I) {}
		Value(uint64_t U) : Kind(Type::Int), IntValue(static_cast<int64_t>(U)) {}
		Value(double D) : Kind(Type::Double), DoubleValue(D) {}
		Value(const char* S) : Kind(Type::String), StringValue(S) {}
		Value(std::string S) : Kind(Type::String), StringValue(std::move(S)) {}
		Value(std::string_view S) : Kind(Type::String), StringValue(S) {}
		Value(Array A);
		Value(Object O);

		Value(const Value& Other);
		Value(Value&& Other) noexcept = default;
		Value& operator=(const Value& Other);
		Value& operator=(Value&& Other) noexcept = default;
		~Value() = default;

		Type GetType() const { return Kind; }
		bool IsNull() const { return Kind == Type::Null; }
		bool IsBool() const { return Kind == Type::Bool; }
		bool IsInt() const { return Kind == Type::Int; }
		bool IsNumber() const { return Kind == Type::Int || Kind == Type::Double; }
		bool IsString() const { return Kind == Type::String; }
		bool IsArray() const { return Kind == Type::List; }
		bool IsObject() const { return Kind == Type::Map; }

		bool AsBool() const { return BoolValue; }
		int64_t AsInt() const { return Kind == Type::Double ? static_cast<int64_t>(DoubleValue) : IntValue; }
		double AsDouble() const { return Kind == Type::Int ? static_cast<double>(IntValue) : DoubleValue; }
		const std::string& AsString() const { return StringValue; }
		const Array& AsArray() const;
		Array& AsArray();
		const Object& AsObject() const;
		Object& AsObject();

		/** Object member lookup; nullptr if not an object or key missing. */
		const Value* Find(std::string_view Key) const;
		/** Sets a member, converting this value to an object if it is null. */
		Value& Set(std::string Key, Value V);

		bool operator==(const Value& Other) const;
		bool operator!=(const Value& Other) const { return !(*this == Other); }

	private:
		Type Kind = Type::Null;
		bool BoolValue = false;
		int64_t IntValue = 0;
		double DoubleValue = 0.0;
		std::string StringValue;
		std::unique_ptr<Array> ArrayValue;
		std::unique_ptr<Object> ObjectValue;
	};

	struct ParseLimits
	{
		size_t MaxBytes = 8 * 1024 * 1024;
		int MaxDepth = 64;
	};

	Result<Value> Parse(std::string_view Text, const ParseLimits& Limits = {});

	/** Compact (Indent < 0) or pretty (Indent spaces) serialisation. */
	std::string Serialize(const Value& V, int Indent = -1);

	/** Checked accessors for decoding untrusted objects. Each returns Invalid with the field name. */
	Result<std::string> GetString(const Value& Obj, std::string_view Key, size_t MaxLen = 4096);
	Result<int64_t> GetInt(const Value& Obj, std::string_view Key);
	Result<bool> GetBool(const Value& Obj, std::string_view Key);
	/** Missing or null -> std::nullopt; present but wrong type -> error. */
	Result<std::optional<std::string>> GetOptionalString(const Value& Obj, std::string_view Key, size_t MaxLen = 4096);
	Result<std::optional<int64_t>> GetOptionalInt(const Value& Obj, std::string_view Key);
	const Value* GetOptional(const Value& Obj, std::string_view Key);
}
