#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdscript {

// R+W data area exported as DEBUG_SYMBOL. Layout: header | frames[MAX_DEPTH].
// Call stack only; line resolution uses the line table on the return address.
// Host zeroes DEPTH_OFF after a fault (guest never runs the pops).
struct DebugLayout {
	static constexpr uint32_t MAGIC = 0x44534447; // 'GDSD'
	static constexpr uint32_t LAYOUT_VERSION = 1;

	// Deeper calls run but are not recorded. Depth keeps counting for balanced exits.
	static constexpr uint32_t MAX_DEPTH = 256;

	// Header. Compiler-written before DEPTH_OFF; guest-written after.
	static constexpr int32_t MAGIC_OFF = 0;
	static constexpr int32_t VERSION_OFF = 4;
	static constexpr int32_t FUNCTION_COUNT_OFF = 8;
	static constexpr int32_t FRAME_SIZE_OFF = 12;
	static constexpr int32_t MAX_DEPTH_OFF = 16;
	static constexpr int32_t DEPTH_OFF = 24;
	static constexpr int32_t HEADER_SIZE = 32;

	// Frame: function index, return address, frame sp. Power-of-two for shift indexing.
	static constexpr int32_t FUNCTION_INDEX_OFF = 0;
	static constexpr int32_t RETURN_ADDRESS_OFF = 8;
	static constexpr int32_t FRAME_SP_OFF = 16;
	static constexpr int32_t INSTANCE_BASE_OFF = 24;
	static constexpr int32_t FRAME_SIZE = 32;
	static constexpr int32_t FRAME_SHIFT = 5;

	static constexpr int32_t frames_offset() { return HEADER_SIZE; }

	static constexpr int32_t frame_offset(uint32_t depth) {
		return frames_offset() + int32_t(depth) * FRAME_SIZE;
	}

	static constexpr int32_t area_size() {
		return frames_offset() + int32_t(MAX_DEPTH) * FRAME_SIZE;
	}
};

inline constexpr const char *DEBUG_SYMBOL = "__gdsc_debug";
inline constexpr const char *DEBUG_GLOBALS_SYMBOL = "__gdsc_globals";

enum class DebugStorage : uint8_t { FRAME, MEMBER, GLOBAL };

struct DebugVariableRecord {
	uint32_t function_index = 0;
	std::string name;
	int32_t type = -1;
	DebugStorage storage = DebugStorage::FRAME;
	int32_t offset = 0;
	uint64_t pc_begin = 0;
	uint64_t pc_end = UINT64_MAX;
};

std::vector<uint8_t> encode_debug_variables(const std::vector<DebugVariableRecord> &records);
bool decode_debug_variables(const uint8_t *data, size_t size,
		std::vector<DebugVariableRecord> &out);

} // namespace gdscript
