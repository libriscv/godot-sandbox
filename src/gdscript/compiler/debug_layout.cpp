#include "debug_layout.h"

#include <cstring>
#include <type_traits>

namespace gdscript {
namespace {
constexpr uint32_t MAGIC = 0x52415644u; // DVAR
constexpr uint16_t MAJOR = 1;
constexpr uint32_t MAX_RECORDS = 100000;
constexpr uint32_t MAX_STRING = 1024 * 1024;
constexpr size_t MAX_BLOB = 16u * 1024u * 1024u;
template <typename T> void put(std::vector<uint8_t> &out, T value) {
	static_assert(std::is_trivially_copyable<T>::value, "raw bytes only");
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(T));
}
void put_string(std::vector<uint8_t> &out, const std::string &value) {
	put<uint32_t>(out, uint32_t(value.size())); out.insert(out.end(), value.begin(), value.end());
}
struct Reader {
	const uint8_t *data; size_t size; size_t at = 0; bool ok = true;
	template <typename T> T get() { T v{}; if (!ok || at > size || sizeof(T) > size - at) { ok = false; return v; } std::memcpy(&v, data + at, sizeof(T)); at += sizeof(T); return v; }
	std::string string() { const uint32_t n = get<uint32_t>(); if (!ok || n > MAX_STRING || at > size || n > size - at) { ok = false; return {}; } std::string v(reinterpret_cast<const char *>(data + at), n); at += n; return v; }
};
}

std::vector<uint8_t> encode_debug_variables(const std::vector<DebugVariableRecord> &records) {
	std::vector<uint8_t> out;
	put<uint32_t>(out, MAGIC); put<uint16_t>(out, MAJOR); put<uint16_t>(out, 0);
	put<uint32_t>(out, uint32_t(records.size()));
	for (const auto &record : records) {
		put<uint32_t>(out, record.function_index); put_string(out, record.name);
		put<int32_t>(out, record.type); put<uint8_t>(out, uint8_t(record.storage));
		put<int32_t>(out, record.offset); put<uint64_t>(out, record.pc_begin); put<uint64_t>(out, record.pc_end);
	}
	return out;
}

bool decode_debug_variables(const uint8_t *data, size_t size,
		std::vector<DebugVariableRecord> &out) {
	out.clear();
	if (data == nullptr || size > MAX_BLOB) return false;
	Reader reader{data, size};
	if (reader.get<uint32_t>() != MAGIC || reader.get<uint16_t>() != MAJOR) return false;
	reader.get<uint16_t>();
	const uint32_t count = reader.get<uint32_t>();
	if (!reader.ok || count > MAX_RECORDS || count > size) return false;
	std::vector<DebugVariableRecord> staged; staged.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		DebugVariableRecord record; record.function_index = reader.get<uint32_t>(); record.name = reader.string();
		record.type = reader.get<int32_t>(); const uint8_t storage = reader.get<uint8_t>();
		record.offset = reader.get<int32_t>(); record.pc_begin = reader.get<uint64_t>(); record.pc_end = reader.get<uint64_t>();
		if (!reader.ok || record.name.empty() || storage > uint8_t(DebugStorage::GLOBAL) ||
				record.type < -1 || record.type > 64 || record.offset < 0 || record.pc_begin > record.pc_end) return false;
		record.storage = DebugStorage(storage); staged.push_back(std::move(record));
	}
	if (!reader.ok || reader.at != size) return false;
	out = std::move(staged); return true;
}
} // namespace gdscript
