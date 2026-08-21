#include "function_signature.h"
#include <cstring>

namespace gdscript {

namespace {

template <typename T>
void write_scalar(std::vector<uint8_t> &out, T value) {
	static_assert(std::is_trivially_copyable<T>::value, "raw bytes only");
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(T));
}

void write_string(std::vector<uint8_t> &out, const std::string &value) {
	write_scalar<uint32_t>(out, uint32_t(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}

// Bounds-checked cursor; truncated/hostile blobs fail the decode.
struct Reader {
	const uint8_t *data;
	size_t size;
	size_t offset = 0;
	bool ok = true;

	template <typename T>
	T scalar() {
		T value {};
		if (!ok || offset + sizeof(T) > size) {
			ok = false;
			return value;
		}
		std::memcpy(&value, data + offset, sizeof(T));
		offset += sizeof(T);
		return value;
	}

	std::string string() {
		const uint32_t length = scalar<uint32_t>();
		if (!ok || offset + length > size) {
			ok = false;
			return {};
		}
		std::string value(reinterpret_cast<const char *>(data + offset), length);
		offset += length;
		return value;
	}
};

} // namespace

std::vector<uint8_t> encode_function_signatures(const std::vector<FunctionSignature> &signatures) {
	std::vector<uint8_t> out;
	write_scalar<uint32_t>(out, uint32_t(signatures.size()));

	for (const FunctionSignature &sig : signatures) {
		write_string(out, sig.name);
		write_scalar<int32_t>(out, sig.return_type);
		write_scalar<int32_t>(out, sig.line);
		write_string(out, sig.description);
		write_scalar<uint32_t>(out, uint32_t(sig.required_arguments));
		write_scalar<uint32_t>(out, uint32_t(sig.parameters.size()));

		for (const FunctionParameter &param : sig.parameters) {
			write_string(out, param.name);
			write_scalar<int32_t>(out, param.type);
			write_scalar<uint8_t>(out, uint8_t(param.default_kind));
			switch (param.default_kind) {
				case FunctionParameter::DefaultKind::INT:
					write_scalar<int64_t>(out, std::get<int64_t>(param.default_value));
					break;
				case FunctionParameter::DefaultKind::FLOAT:
					write_scalar<double>(out, std::get<double>(param.default_value));
					break;
				case FunctionParameter::DefaultKind::BOOL:
					write_scalar<uint8_t>(out, std::get<bool>(param.default_value) ? 1 : 0);
					break;
				case FunctionParameter::DefaultKind::STRING:
					write_string(out, std::get<std::string>(param.default_value));
					break;
				case FunctionParameter::DefaultKind::NONE:
				case FunctionParameter::DefaultKind::NIL:
				case FunctionParameter::DefaultKind::EMPTY_ARRAY:
				case FunctionParameter::DefaultKind::EMPTY_DICT:
					break;
			}
		}
	}
	return out;
}

bool decode_function_signatures(const uint8_t *data, size_t size,
	std::vector<FunctionSignature> &out)
{
	out.clear();
	Reader reader{ data, size };

	const uint32_t function_count = reader.scalar<uint32_t>();
	// Count larger than blob size cannot be honest; prevents corrupt header from reserving.
	if (!reader.ok || function_count > size) {
		out.clear();
		return false;
	}
	out.reserve(function_count);

	for (uint32_t i = 0; i < function_count; i++) {
		FunctionSignature sig;
		sig.name = reader.string();
		sig.return_type = reader.scalar<int32_t>();
		sig.line = reader.scalar<int32_t>();
		sig.description = reader.string();
		sig.required_arguments = reader.scalar<uint32_t>();
		const uint32_t parameter_count = reader.scalar<uint32_t>();
		if (!reader.ok || parameter_count > size) {
			out.clear();
			return false;
		}

		for (uint32_t p = 0; p < parameter_count; p++) {
			FunctionParameter param;
			param.name = reader.string();
			param.type = reader.scalar<int32_t>();
			const uint8_t kind = reader.scalar<uint8_t>();
			if (!reader.ok || kind > uint8_t(FunctionParameter::DefaultKind::EMPTY_DICT)) {
				out.clear();
				return false;
			}
			param.default_kind = FunctionParameter::DefaultKind(kind);
			switch (param.default_kind) {
				case FunctionParameter::DefaultKind::INT:
					param.default_value = reader.scalar<int64_t>();
					break;
				case FunctionParameter::DefaultKind::FLOAT:
					param.default_value = reader.scalar<double>();
					break;
				case FunctionParameter::DefaultKind::BOOL:
					param.default_value = reader.scalar<uint8_t>() != 0;
					break;
				case FunctionParameter::DefaultKind::STRING:
					param.default_value = reader.string();
					break;
				case FunctionParameter::DefaultKind::NONE:
				case FunctionParameter::DefaultKind::NIL:
				case FunctionParameter::DefaultKind::EMPTY_ARRAY:
				case FunctionParameter::DefaultKind::EMPTY_DICT:
					break;
			}
			sig.parameters.push_back(std::move(param));
		}

		// Clamp rather than trust: out-of-range would let callers skip arguments.
		if (sig.required_arguments > sig.parameters.size()) {
			sig.required_arguments = sig.parameters.size();
		}
		out.push_back(std::move(sig));
	}

	if (!reader.ok) {
		out.clear();
		return false;
	}
	return true;
}

} // namespace gdscript
