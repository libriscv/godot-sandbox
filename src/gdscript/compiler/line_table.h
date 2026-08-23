#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gdscript {

// Address-to-source-line mapping. Metadata only; produced by every compile.
struct LineTableEntry {
	uint32_t address; // text offset from the ELF base
	uint32_t line; // 1-based; 0 where the compiler knows of no line
};

// Ascending address order. A row's line covers up to the next row.
struct LineTable {
	std::vector<LineTableEntry> entries;

	// 0 if address precedes all rows or table is empty.
	uint32_t line_for_address(uint32_t address) const;

	// Lowest address mapped to `line`, or 0 if absent.
	uint32_t address_for_line(uint32_t line) const;

	// No duplicate addresses, no consecutive rows with the same line.
	bool is_normalized() const;
};

// Single blob encoding (like signatures): avoids per-row scoped variants.
std::vector<uint8_t> encode_line_table(const LineTable &table);

// Returns false on truncated, misordered or invalid blobs; output left empty.
bool decode_line_table(const uint8_t *data, size_t size, LineTable &out);

} // namespace gdscript
