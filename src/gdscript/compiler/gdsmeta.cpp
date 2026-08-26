#include "gdsmeta.h"
#include <cstring>

namespace gdscript {

namespace {

constexpr uint32_t GDSMETA_MAGIC = 0x4D534447;
constexpr uint16_t GDSMETA_VERSION = 1;

constexpr uint16_t FLAG_DOUBLE_PRECISION = 1 << 0;
constexpr uint16_t FLAG_IS_TOOL = 1 << 1;
constexpr uint16_t FLAG_BASE_IS_PATH = 1 << 2;

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

void write_blob(std::vector<uint8_t> &out, const std::vector<uint8_t> &blob) {
	write_scalar<uint32_t>(out, uint32_t(blob.size()));
	out.insert(out.end(), blob.begin(), blob.end());
}

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

	std::pair<const uint8_t *, size_t> blob() {
		const uint32_t length = scalar<uint32_t>();
		if (!ok || offset + length > size) {
			ok = false;
			return { nullptr, 0 };
		}
		const uint8_t *ptr = data + offset;
		offset += length;
		return { ptr, length };
	}
};

} // namespace

// Layout:
//   u32 magic 'GDSM'
//   u16 version
//   u16 flags (double_precision | is_tool | base_is_path)
//   str class_name
//   str base_class
//   blob functions   (encode_function_signatures)
//   blob signals     (encode_function_signatures)
//   blob line_table  (encode_line_table)
std::vector<uint8_t> encode_script_metadata(const ScriptMetadata &meta) {
	std::vector<uint8_t> out;
	write_scalar<uint32_t>(out, GDSMETA_MAGIC);
	write_scalar<uint16_t>(out, GDSMETA_VERSION);

	uint16_t flags = 0;
	if (meta.double_precision) {
		flags |= FLAG_DOUBLE_PRECISION;
	}
	if (meta.is_tool) {
		flags |= FLAG_IS_TOOL;
	}
	if (meta.base_is_path) {
		flags |= FLAG_BASE_IS_PATH;
	}
	write_scalar<uint16_t>(out, flags);

	write_string(out, meta.class_name);
	write_string(out, meta.base_class);

	write_blob(out, encode_function_signatures(meta.functions));
	write_blob(out, encode_function_signatures(meta.signals));
	write_blob(out, encode_line_table(meta.line_table));

	return out;
}

bool decode_script_metadata(const uint8_t *data, size_t size, ScriptMetadata &out) {
	out = ScriptMetadata{};

	Reader reader{ data, size };
	const uint32_t magic = reader.scalar<uint32_t>();
	const uint16_t version = reader.scalar<uint16_t>();
	if (!reader.ok || magic != GDSMETA_MAGIC || version != GDSMETA_VERSION) {
		return false;
	}

	const uint16_t flags = reader.scalar<uint16_t>();
	out.double_precision = (flags & FLAG_DOUBLE_PRECISION) != 0;
	out.is_tool = (flags & FLAG_IS_TOOL) != 0;
	out.base_is_path = (flags & FLAG_BASE_IS_PATH) != 0;

	out.class_name = reader.string();
	out.base_class = reader.string();

	const auto functions_blob = reader.blob();
	const auto signals_blob = reader.blob();
	const auto line_table_blob = reader.blob();
	if (!reader.ok) {
		out = ScriptMetadata{};
		return false;
	}

	if (!decode_function_signatures(functions_blob.first, functions_blob.second, out.functions) ||
		!decode_function_signatures(signals_blob.first, signals_blob.second, out.signals) ||
		!decode_line_table(line_table_blob.first, line_table_blob.second, out.line_table)) {
		out = ScriptMetadata{};
		return false;
	}

	return true;
}

} // namespace gdscript
