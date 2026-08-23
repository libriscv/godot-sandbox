#include "line_table.h"
#include <algorithm>
#include <cstring>

namespace gdscript {

namespace {

constexpr uint32_t LINE_TABLE_MAGIC = 0x4C534447; // 'GDSL'
constexpr uint32_t LINE_TABLE_VERSION = 1;

template <typename T>
void write_scalar(std::vector<uint8_t> &out, T value) {
	static_assert(std::is_trivially_copyable<T>::value, "raw bytes only");
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(T));
}

} // namespace

uint32_t LineTable::line_for_address(uint32_t address) const {
	// upper_bound - 1 gives the covering row.
	const auto it = std::upper_bound(entries.begin(), entries.end(), address,
		[](uint32_t value, const LineTableEntry &entry) { return value < entry.address; });
	if (it == entries.begin()) {
		return 0;
	}
	return std::prev(it)->line;
}

uint32_t LineTable::address_for_line(uint32_t line) const {
	// Linear scan; lowest address wins when a line appears in multiple rows.
	uint32_t best = 0;
	bool found = false;
	for (const LineTableEntry &entry : entries) {
		if (entry.line != line) {
			continue;
		}
		if (!found || entry.address < best) {
			best = entry.address;
			found = true;
		}
	}
	return found ? best : 0;
}

bool LineTable::is_normalized() const {
	for (size_t i = 1; i < entries.size(); i++) {
		if (entries[i].address <= entries[i - 1].address) {
			return false;
		}
		if (entries[i].line == entries[i - 1].line) {
			return false;
		}
	}
	return true;
}

std::vector<uint8_t> encode_line_table(const LineTable &table) {
	std::vector<uint8_t> out;
	out.reserve(12 + table.entries.size() * 8);
	write_scalar<uint32_t>(out, LINE_TABLE_MAGIC);
	write_scalar<uint32_t>(out, LINE_TABLE_VERSION);
	write_scalar<uint32_t>(out, uint32_t(table.entries.size()));

	for (const LineTableEntry &entry : table.entries) {
		write_scalar<uint32_t>(out, entry.address);
		write_scalar<uint32_t>(out, entry.line);
	}
	return out;
}

bool decode_line_table(const uint8_t *data, size_t size, LineTable &out) {
	out.entries.clear();
	if (data == nullptr || size < 12) {
		return false;
	}

	uint32_t header[3];
	std::memcpy(header, data, sizeof(header));
	if (header[0] != LINE_TABLE_MAGIC || header[1] != LINE_TABLE_VERSION) {
		return false;
	}

	const uint32_t count = header[2];
	// Exact size match; no trailing data allowed.
	if (size != 12 + size_t(count) * 8) {
		return false;
	}

	out.entries.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		uint32_t row[2];
		std::memcpy(row, data + 12 + size_t(i) * 8, sizeof(row));
		out.entries[i].address = row[0];
		out.entries[i].line = row[1];
	}

	// Ascending address order required for binary search.
	for (uint32_t i = 1; i < count; i++) {
		if (out.entries[i].address <= out.entries[i - 1].address) {
			out.entries.clear();
			return false;
		}
	}
	return true;
}

} // namespace gdscript
