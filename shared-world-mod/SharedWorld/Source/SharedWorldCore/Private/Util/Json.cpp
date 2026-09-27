#include "SharedWorldCore/Util/Json.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sw::json
{
	Value::Value(Array A) : Kind(Type::List), ArrayValue(std::make_unique<Array>(std::move(A))) {}
	Value::Value(Object O) : Kind(Type::Map), ObjectValue(std::make_unique<Object>(std::move(O))) {}

	Value::Value(const Value& Other)
		: Kind(Other.Kind), BoolValue(Other.BoolValue), IntValue(Other.IntValue), DoubleValue(Other.DoubleValue), StringValue(Other.StringValue)
	{
		if (Other.ArrayValue)
		{
			ArrayValue = std::make_unique<Array>(*Other.ArrayValue);
		}
		if (Other.ObjectValue)
		{
			ObjectValue = std::make_unique<Object>(*Other.ObjectValue);
		}
	}

	Value& Value::operator=(const Value& Other)
	{
		if (this != &Other)
		{
			Value Copy(Other);
			*this = std::move(Copy);
		}
		return *this;
	}

	static const Array EmptyArray;
	static const Object EmptyObject;

	const Array& Value::AsArray() const { return ArrayValue ? *ArrayValue : EmptyArray; }
	Array& Value::AsArray()
	{
		if (!ArrayValue)
		{
			Kind = Type::List;
			ArrayValue = std::make_unique<Array>();
		}
		return *ArrayValue;
	}
	const Object& Value::AsObject() const { return ObjectValue ? *ObjectValue : EmptyObject; }
	Object& Value::AsObject()
	{
		if (!ObjectValue)
		{
			Kind = Type::Map;
			ObjectValue = std::make_unique<Object>();
		}
		return *ObjectValue;
	}

	const Value* Value::Find(std::string_view Key) const
	{
		if (Kind != Type::Map || !ObjectValue)
		{
			return nullptr;
		}
		auto It = ObjectValue->find(Key);
		return It == ObjectValue->end() ? nullptr : &It->second;
	}

	Value& Value::Set(std::string Key, Value V)
	{
		Object& O = AsObject();
		return O.insert_or_assign(std::move(Key), std::move(V)).first->second;
	}

	bool Value::operator==(const Value& Other) const
	{
		if (IsNumber() && Other.IsNumber())
		{
			if (Kind == Type::Int && Other.Kind == Type::Int)
			{
				return IntValue == Other.IntValue;
			}
			return AsDouble() == Other.AsDouble();
		}
		if (Kind != Other.Kind)
		{
			return false;
		}
		switch (Kind)
		{
		case Type::Null: return true;
		case Type::Bool: return BoolValue == Other.BoolValue;
		case Type::String: return StringValue == Other.StringValue;
		case Type::List: return AsArray() == Other.AsArray();
		case Type::Map: return AsObject() == Other.AsObject();
		default: return false;
		}
	}

	// ------------------------------------------------------------------ parser

	namespace
	{
		struct Parser
		{
			std::string_view S;
			size_t Pos = 0;
			int Depth = 0;
			int MaxDepth = 64;
			std::string ErrorMessage;

			bool Fail(const char* Msg)
			{
				if (ErrorMessage.empty())
				{
					ErrorMessage = std::string(Msg) + " at offset " + std::to_string(Pos);
				}
				return false;
			}

			void SkipWs()
			{
				while (Pos < S.size() && (S[Pos] == ' ' || S[Pos] == '\t' || S[Pos] == '\n' || S[Pos] == '\r'))
				{
					++Pos;
				}
			}

			bool Literal(const char* Lit)
			{
				size_t N = std::strlen(Lit);
				if (S.substr(Pos, N) != Lit)
				{
					return Fail("invalid literal");
				}
				Pos += N;
				return true;
			}

			static void AppendUtf8(std::string& Out, uint32_t Cp)
			{
				if (Cp < 0x80)
				{
					Out += static_cast<char>(Cp);
				}
				else if (Cp < 0x800)
				{
					Out += static_cast<char>(0xC0 | (Cp >> 6));
					Out += static_cast<char>(0x80 | (Cp & 0x3F));
				}
				else if (Cp < 0x10000)
				{
					Out += static_cast<char>(0xE0 | (Cp >> 12));
					Out += static_cast<char>(0x80 | ((Cp >> 6) & 0x3F));
					Out += static_cast<char>(0x80 | (Cp & 0x3F));
				}
				else
				{
					Out += static_cast<char>(0xF0 | (Cp >> 18));
					Out += static_cast<char>(0x80 | ((Cp >> 12) & 0x3F));
					Out += static_cast<char>(0x80 | ((Cp >> 6) & 0x3F));
					Out += static_cast<char>(0x80 | (Cp & 0x3F));
				}
			}

			bool Hex4(uint32_t& Out)
			{
				if (Pos + 4 > S.size())
				{
					return Fail("truncated \\u escape");
				}
				Out = 0;
				for (int i = 0; i < 4; ++i)
				{
					char C = S[Pos++];
					Out <<= 4;
					if (C >= '0' && C <= '9') Out |= static_cast<uint32_t>(C - '0');
					else if (C >= 'a' && C <= 'f') Out |= static_cast<uint32_t>(C - 'a' + 10);
					else if (C >= 'A' && C <= 'F') Out |= static_cast<uint32_t>(C - 'A' + 10);
					else return Fail("bad hex digit");
				}
				return true;
			}

			// Validates one UTF-8 sequence starting at Pos and appends it.
			bool Utf8Char(std::string& Out)
			{
				const unsigned char C = static_cast<unsigned char>(S[Pos]);
				int Len = 0;
				uint32_t Cp = 0;
				if (C < 0x80) { Out += static_cast<char>(C); ++Pos; return true; }
				if ((C & 0xE0) == 0xC0) { Len = 2; Cp = C & 0x1F; }
				else if ((C & 0xF0) == 0xE0) { Len = 3; Cp = C & 0x0F; }
				else if ((C & 0xF8) == 0xF0) { Len = 4; Cp = C & 0x07; }
				else return Fail("invalid UTF-8");
				if (Pos + Len > S.size()) return Fail("truncated UTF-8");
				for (int i = 1; i < Len; ++i)
				{
					const unsigned char K = static_cast<unsigned char>(S[Pos + i]);
					if ((K & 0xC0) != 0x80) return Fail("invalid UTF-8");
					Cp = (Cp << 6) | (K & 0x3F);
				}
				// Reject overlong forms, surrogates and out-of-range code points.
				if ((Len == 2 && Cp < 0x80) || (Len == 3 && Cp < 0x800) || (Len == 4 && Cp < 0x10000) || Cp > 0x10FFFF || (Cp >= 0xD800 && Cp <= 0xDFFF))
				{
					return Fail("invalid UTF-8");
				}
				Out.append(S.substr(Pos, Len));
				Pos += Len;
				return true;
			}

			bool ParseString(std::string& Out)
			{
				++Pos; // opening quote
				while (true)
				{
					if (Pos >= S.size()) return Fail("unterminated string");
					char C = S[Pos];
					if (C == '"') { ++Pos; return true; }
					if (static_cast<unsigned char>(C) < 0x20) return Fail("control character in string");
					if (C == '\\')
					{
						++Pos;
						if (Pos >= S.size()) return Fail("truncated escape");
						char E = S[Pos++];
						switch (E)
						{
						case '"': Out += '"'; break;
						case '\\': Out += '\\'; break;
						case '/': Out += '/'; break;
						case 'b': Out += '\b'; break;
						case 'f': Out += '\f'; break;
						case 'n': Out += '\n'; break;
						case 'r': Out += '\r'; break;
						case 't': Out += '\t'; break;
						case 'u':
						{
							uint32_t Cp = 0;
							if (!Hex4(Cp)) return false;
							if (Cp >= 0xD800 && Cp <= 0xDBFF)
							{
								uint32_t Lo = 0;
								if (S.substr(Pos, 2) != "\\u") return Fail("unpaired surrogate");
								Pos += 2;
								if (!Hex4(Lo) || Lo < 0xDC00 || Lo > 0xDFFF) return Fail("unpaired surrogate");
								Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
							}
							else if (Cp >= 0xDC00 && Cp <= 0xDFFF)
							{
								return Fail("unpaired surrogate");
							}
							AppendUtf8(Out, Cp);
							break;
						}
						default: return Fail("invalid escape");
						}
						continue;
					}
					if (!Utf8Char(Out)) return false;
				}
			}

			bool ParseNumber(Value& Out)
			{
				const size_t Start = Pos;
				if (S[Pos] == '-') ++Pos;
				if (Pos >= S.size()) return Fail("bad number");
				if (S[Pos] == '0') { ++Pos; }
				else if (S[Pos] >= '1' && S[Pos] <= '9') { while (Pos < S.size() && std::isdigit(static_cast<unsigned char>(S[Pos]))) ++Pos; }
				else return Fail("bad number");
				bool bInteger = true;
				if (Pos < S.size() && S[Pos] == '.')
				{
					bInteger = false;
					++Pos;
					if (Pos >= S.size() || !std::isdigit(static_cast<unsigned char>(S[Pos]))) return Fail("bad fraction");
					while (Pos < S.size() && std::isdigit(static_cast<unsigned char>(S[Pos]))) ++Pos;
				}
				if (Pos < S.size() && (S[Pos] == 'e' || S[Pos] == 'E'))
				{
					bInteger = false;
					++Pos;
					if (Pos < S.size() && (S[Pos] == '+' || S[Pos] == '-')) ++Pos;
					if (Pos >= S.size() || !std::isdigit(static_cast<unsigned char>(S[Pos]))) return Fail("bad exponent");
					while (Pos < S.size() && std::isdigit(static_cast<unsigned char>(S[Pos]))) ++Pos;
				}
				const char* First = S.data() + Start;
				const char* Last = S.data() + Pos;
				if (bInteger)
				{
					int64_t I = 0;
					auto [Ptr, Ec] = std::from_chars(First, Last, I);
					if (Ec == std::errc() && Ptr == Last)
					{
						Out = Value(I);
						return true;
					}
					// Out of int64 range: fall through to double.
				}
				// std::from_chars for double is not available on every
				// toolchain Unreal uses; strtod on a bounded copy is.
				std::string Copy(First, Last);
				char* End = nullptr;
				double D = std::strtod(Copy.c_str(), &End);
				if (End != Copy.c_str() + Copy.size() || !std::isfinite(D)) return Fail("bad number");
				Out = Value(D);
				return true;
			}

			bool ParseValue(Value& Out)
			{
				SkipWs();
				if (Pos >= S.size()) return Fail("unexpected end of input");
				const char C = S[Pos];
				switch (C)
				{
				case 'n': if (!Literal("null")) return false; Out = Value(); return true;
				case 't': if (!Literal("true")) return false; Out = Value(true); return true;
				case 'f': if (!Literal("false")) return false; Out = Value(false); return true;
				case '"': { std::string Str; if (!ParseString(Str)) return false; Out = Value(std::move(Str)); return true; }
				case '[': return ParseArray(Out);
				case '{': return ParseObject(Out);
				default:
					if (C == '-' || (C >= '0' && C <= '9')) return ParseNumber(Out);
					return Fail("unexpected character");
				}
			}

			bool ParseArray(Value& Out)
			{
				if (++Depth > MaxDepth) return Fail("nesting too deep");
				++Pos;
				Array A;
				SkipWs();
				if (Pos < S.size() && S[Pos] == ']') { ++Pos; --Depth; Out = Value(std::move(A)); return true; }
				while (true)
				{
					Value Item;
					if (!ParseValue(Item)) return false;
					A.push_back(std::move(Item));
					SkipWs();
					if (Pos >= S.size()) return Fail("unterminated array");
					if (S[Pos] == ',') { ++Pos; continue; }
					if (S[Pos] == ']') { ++Pos; break; }
					return Fail("expected , or ]");
				}
				--Depth;
				Out = Value(std::move(A));
				return true;
			}

			bool ParseObject(Value& Out)
			{
				if (++Depth > MaxDepth) return Fail("nesting too deep");
				++Pos;
				Object O;
				SkipWs();
				if (Pos < S.size() && S[Pos] == '}') { ++Pos; --Depth; Out = Value(std::move(O)); return true; }
				while (true)
				{
					SkipWs();
					if (Pos >= S.size() || S[Pos] != '"') return Fail("expected key");
					std::string Key;
					if (!ParseString(Key)) return false;
					SkipWs();
					if (Pos >= S.size() || S[Pos] != ':') return Fail("expected :");
					++Pos;
					Value Item;
					if (!ParseValue(Item)) return false;
					if (!O.emplace(std::move(Key), std::move(Item)).second) return Fail("duplicate key");
					SkipWs();
					if (Pos >= S.size()) return Fail("unterminated object");
					if (S[Pos] == ',') { ++Pos; continue; }
					if (S[Pos] == '}') { ++Pos; break; }
					return Fail("expected , or }");
				}
				--Depth;
				Out = Value(std::move(O));
				return true;
			}
		};

		void WriteString(std::string& Out, const std::string& Str)
		{
			Out += '"';
			for (unsigned char C : Str)
			{
				switch (C)
				{
				case '"': Out += "\\\""; break;
				case '\\': Out += "\\\\"; break;
				case '\n': Out += "\\n"; break;
				case '\r': Out += "\\r"; break;
				case '\t': Out += "\\t"; break;
				case '\b': Out += "\\b"; break;
				case '\f': Out += "\\f"; break;
				default:
					if (C < 0x20)
					{
						char Buf[8];
						std::snprintf(Buf, sizeof(Buf), "\\u%04x", C);
						Out += Buf;
					}
					else
					{
						Out += static_cast<char>(C);
					}
				}
			}
			Out += '"';
		}

		void Write(std::string& Out, const Value& V, int Indent, int Level)
		{
			auto Newline = [&](int L)
			{
				if (Indent >= 0)
				{
					Out += '\n';
					Out.append(static_cast<size_t>(Indent * L), ' ');
				}
			};
			switch (V.GetType())
			{
			case Type::Null: Out += "null"; break;
			case Type::Bool: Out += V.AsBool() ? "true" : "false"; break;
			case Type::Int: Out += std::to_string(V.AsInt()); break;
			case Type::Double:
			{
				char Buf[32];
				std::snprintf(Buf, sizeof(Buf), "%.17g", V.AsDouble());
				Out += Buf;
				break;
			}
			case Type::String: WriteString(Out, V.AsString()); break;
			case Type::List:
			{
				const Array& A = V.AsArray();
				Out += '[';
				for (size_t i = 0; i < A.size(); ++i)
				{
					if (i) Out += ',';
					Newline(Level + 1);
					Write(Out, A[i], Indent, Level + 1);
				}
				if (!A.empty()) Newline(Level);
				Out += ']';
				break;
			}
			case Type::Map:
			{
				const Object& O = V.AsObject();
				Out += '{';
				bool bFirst = true;
				for (const auto& [K, Item] : O)
				{
					if (!bFirst) Out += ',';
					bFirst = false;
					Newline(Level + 1);
					WriteString(Out, K);
					Out += Indent >= 0 ? ": " : ":";
					Write(Out, Item, Indent, Level + 1);
				}
				if (!O.empty()) Newline(Level);
				Out += '}';
				break;
			}
			}
		}
	}

	Result<Value> Parse(std::string_view Text, const ParseLimits& Limits)
	{
		if (Text.size() > Limits.MaxBytes)
		{
			return MakeError(ErrorCode::Invalid, "JSON document too large");
		}
		Parser P;
		P.S = Text;
		P.MaxDepth = Limits.MaxDepth;
		Value V;
		if (!P.ParseValue(V))
		{
			return MakeError(ErrorCode::Invalid, "invalid JSON: " + P.ErrorMessage);
		}
		P.SkipWs();
		if (P.Pos != Text.size())
		{
			return MakeError(ErrorCode::Invalid, "invalid JSON: trailing data at offset " + std::to_string(P.Pos));
		}
		return V;
	}

	std::string Serialize(const Value& V, int Indent)
	{
		std::string Out;
		Write(Out, V, Indent, 0);
		return Out;
	}

	Result<std::string> GetString(const Value& Obj, std::string_view Key, size_t MaxLen)
	{
		const Value* V = Obj.Find(Key);
		if (!V || !V->IsString())
		{
			return MakeError(ErrorCode::Invalid, "field '" + std::string(Key) + "' must be a string");
		}
		if (V->AsString().size() > MaxLen)
		{
			return MakeError(ErrorCode::Invalid, "field '" + std::string(Key) + "' is too long");
		}
		return V->AsString();
	}

	Result<int64_t> GetInt(const Value& Obj, std::string_view Key)
	{
		const Value* V = Obj.Find(Key);
		if (!V || !V->IsInt())
		{
			return MakeError(ErrorCode::Invalid, "field '" + std::string(Key) + "' must be an integer");
		}
		return V->AsInt();
	}

	Result<bool> GetBool(const Value& Obj, std::string_view Key)
	{
		const Value* V = Obj.Find(Key);
		if (!V || !V->IsBool())
		{
			return MakeError(ErrorCode::Invalid, "field '" + std::string(Key) + "' must be a boolean");
		}
		return V->AsBool();
	}

	const Value* GetOptional(const Value& Obj, std::string_view Key)
	{
		const Value* V = Obj.Find(Key);
		return (V && !V->IsNull()) ? V : nullptr;
	}

	Result<std::optional<std::string>> GetOptionalString(const Value& Obj, std::string_view Key, size_t MaxLen)
	{
		if (!GetOptional(Obj, Key))
		{
			return std::optional<std::string>();
		}
		auto S = GetString(Obj, Key, MaxLen);
		if (!S) return S.Err();
		return std::optional<std::string>(std::move(S.Value()));
	}

	Result<std::optional<int64_t>> GetOptionalInt(const Value& Obj, std::string_view Key)
	{
		if (!GetOptional(Obj, Key))
		{
			return std::optional<int64_t>();
		}
		auto I = GetInt(Obj, Key);
		if (!I) return I.Err();
		return std::optional<int64_t>(I.Value());
	}
}
