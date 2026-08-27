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
		write_scalar<uint8_t>(out, sig.is_coroutine ? 1 : 0);
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
		sig.is_coroutine = reader.scalar<uint8_t>() != 0;
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

std::vector<uint8_t> encode_class_signatures(const std::vector<ClassSignature> &classes) {
	std::vector<uint8_t> out;
	write_scalar<uint32_t>(out, uint32_t(classes.size()));

	for (const ClassSignature &cls : classes) {
		write_string(out, cls.name);
		write_string(out, cls.base_name);
		write_string(out, cls.native_base);
		write_scalar<int32_t>(out, cls.line);
		write_scalar<uint32_t>(out, uint32_t(cls.fields.size()));
		for (const ClassField &field : cls.fields) {
			write_string(out, field.name);
			write_scalar<int32_t>(out, field.type);
		}
		write_scalar<uint32_t>(out, uint32_t(cls.methods.size()));
		for (const ClassMethod &method : cls.methods) {
			write_string(out, method.name);
			write_scalar<uint8_t>(out, method.is_static ? 1 : 0);
		}
	}
	return out;
}

bool decode_class_signatures(const uint8_t *data, size_t size,
	std::vector<ClassSignature> &out)
{
	out.clear();
	Reader reader{ data, size };

	const uint32_t class_count = reader.scalar<uint32_t>();
	if (!reader.ok || class_count > size) {
		out.clear();
		return false;
	}
	out.reserve(class_count);

	for (uint32_t i = 0; i < class_count; i++) {
		ClassSignature cls;
		cls.name = reader.string();
		cls.base_name = reader.string();
		cls.native_base = reader.string();
		cls.line = reader.scalar<int32_t>();
		const uint32_t field_count = reader.scalar<uint32_t>();
		if (!reader.ok || field_count > size) {
			out.clear();
			return false;
		}
		for (uint32_t f = 0; f < field_count; f++) {
			ClassField field;
			field.name = reader.string();
			field.type = reader.scalar<int32_t>();
			cls.fields.push_back(std::move(field));
		}
		const uint32_t method_count = reader.scalar<uint32_t>();
		if (!reader.ok || method_count > size) {
			out.clear();
			return false;
		}
		for (uint32_t m = 0; m < method_count; m++) {
			ClassMethod method;
			method.name = reader.string();
			method.is_static = reader.scalar<uint8_t>() != 0;
			cls.methods.push_back(std::move(method));
		}
		out.push_back(std::move(cls));
	}

	if (!reader.ok) {
		out.clear();
		return false;
	}
	return true;
}

std::vector<uint8_t> encode_script_constants(const std::vector<ScriptConstant> &constants) {
	std::vector<uint8_t> out;
	write_scalar<uint32_t>(out, uint32_t(constants.size()));

	for (const ScriptConstant &constant : constants) {
		write_string(out, constant.name);
		write_scalar<uint8_t>(out, uint8_t(constant.kind));
		switch (constant.kind) {
			case ScriptConstant::Kind::INT:
				write_scalar<int64_t>(out, std::get<int64_t>(constant.value));
				break;
			case ScriptConstant::Kind::FLOAT:
				write_scalar<double>(out, std::get<double>(constant.value));
				break;
			case ScriptConstant::Kind::BOOL:
				write_scalar<uint8_t>(out, std::get<bool>(constant.value) ? 1 : 0);
				break;
			case ScriptConstant::Kind::STRING:
				write_string(out, std::get<std::string>(constant.value));
				break;
			case ScriptConstant::Kind::ENUM:
				write_scalar<uint32_t>(out, uint32_t(constant.members.size()));
				for (const ScriptConstant::EnumMember &member : constant.members) {
					write_string(out, member.name);
					write_scalar<int64_t>(out, member.value);
				}
				break;
		}
	}
	return out;
}

bool decode_script_constants(const uint8_t *data, size_t size,
	std::vector<ScriptConstant> &out)
{
	out.clear();
	Reader reader{ data, size };

	const uint32_t count = reader.scalar<uint32_t>();
	if (!reader.ok || count > size) {
		out.clear();
		return false;
	}
	out.reserve(count);

	for (uint32_t i = 0; i < count; i++) {
		ScriptConstant constant;
		constant.name = reader.string();
		const uint8_t kind = reader.scalar<uint8_t>();
		if (!reader.ok || kind > uint8_t(ScriptConstant::Kind::ENUM)) {
			out.clear();
			return false;
		}
		constant.kind = ScriptConstant::Kind(kind);
		switch (constant.kind) {
			case ScriptConstant::Kind::INT:
				constant.value = reader.scalar<int64_t>();
				break;
			case ScriptConstant::Kind::FLOAT:
				constant.value = reader.scalar<double>();
				break;
			case ScriptConstant::Kind::BOOL:
				constant.value = reader.scalar<uint8_t>() != 0;
				break;
			case ScriptConstant::Kind::STRING:
				constant.value = reader.string();
				break;
			case ScriptConstant::Kind::ENUM: {
				const uint32_t member_count = reader.scalar<uint32_t>();
				if (!reader.ok || member_count > size) {
					out.clear();
					return false;
				}
				for (uint32_t m = 0; m < member_count; m++) {
					ScriptConstant::EnumMember member;
					member.name = reader.string();
					member.value = reader.scalar<int64_t>();
					constant.members.push_back(std::move(member));
				}
				break;
			}
		}
		out.push_back(std::move(constant));
	}

	if (!reader.ok) {
		out.clear();
		return false;
	}
	return true;
}

} // namespace gdscript
